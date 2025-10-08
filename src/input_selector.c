/* Copyright LiveTimeNet, Inc. 2021. All Rights Reserved. */

/*
 tstools_bitrate_smoother -i udp://127.0.0.1:4001?buffer_size=250000 \
                          -o udp://127.0.0.1:4002?pkt_size=1316 -b 20000000 -P 0x500
 */
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <curses.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <stdatomic.h>
#include <bitstream/mpeg/ts.h>
#include <bitstream/ietf/rtp.h>
#include <libltntstools/ltntstools.h>
#include "ffmpeg-includes.h"
#include "kbhit.h"
#include "utils.h"

#define DEFAULT_LATENCY 100

char *strcasestr(const char *haystack, const char *needle);

/* We previously had ENABLE_PIR_CORRECTOR disabled code here,
 * that demonstrated patching PIR streams.
 * It was non-functional and removed, but if you want it, check out
 * hash 087d51a10d46d11e33a71c95d8f6934793f71b1f
 */

static int gRunning = 0;

struct tool_input_context_s
{
	char *iname;

	int buflen;
	unsigned char *buf;
	int bytes_available;

	struct rtp_hdr_analyzer_s rtp_stream_in;

	struct ltntstools_stream_statistics_s *i_stream;

	void *sm; /* StreamModel Context */
	int smcomplete;

	AVIOContext *i_puc;
	bool verbose;

	bool isRTP;
};

int tool_input_free(struct tool_input_context_s **ctxp)
{
	struct tool_input_context_s *ctx = *ctxp;
	free(ctx->buf);
	avio_close(ctx->i_puc);
	ltntstools_pid_stats_free(ctx->i_stream);
	ltntstools_streammodel_free(ctx->i_stream);
	free(ctx);
	*ctxp = NULL;
	return 0;
}

static void signal_handler(int signum)
{
	gRunning = 0;
}

static void kernel_check_socket_sizes(AVIOContext *i)
{
	printf("Kernel configured default/max socket buffer sizes:\n");

	char line[256];
	int val;
	FILE *fh = fopen("/proc/sys/net/core/rmem_default", "r");
	if (fh) {
		fread(&line[0], 1, sizeof(line), fh);
		val = atoi(line);
		printf("/proc/sys/net/core/rmem_default = %d\n", val);
		fclose(fh);
	}

	fh = fopen("/proc/sys/net/core/rmem_max", "r");
	if (fh) {
		fread(&line[0], 1, sizeof(line), fh);
		val = atoi(line);
		printf("/proc/sys/net/core/rmem_max = %d\n", val);
		if (i->buffer_size > val) {
			fprintf(stderr, "buffer_size %d exceeds rmem_max %d, aborting\n", i->buffer_size, val);
			exit(1);
		}
		fclose(fh);
	}

}

static struct tool_input_context_s * tool_input_new(char *url) {
	struct tool_input_context_s *ctx = calloc(1, sizeof(struct tool_input_context_s));

	if (!ctx) return NULL;

	ctx->iname = url;

	ctx->buflen = 188 * 1024;

	if (!(ctx->buf = malloc(ctx->buflen))) {
		tool_input_free(&ctx);
	}

	else if (ltntstools_pid_stats_alloc(&ctx->i_stream) != 0) {
		tool_input_free(&ctx);
	}

	else if (ltntstools_streammodel_alloc(&ctx->sm, NULL) < 0) {
		fprintf(stderr, "\nUnable to allocate streammodel object.\n");
		tool_input_free(&ctx);
	}

	rtp_analyzer_init(&ctx->rtp_stream_in);

	return ctx;
}


static int tool_input_open(struct tool_input_context_s *ctx) {
	int ret = avio_open2(&ctx->i_puc, ctx->iname, AVIO_FLAG_READ | AVIO_FLAG_NONBLOCK | AVIO_FLAG_DIRECT, NULL, NULL);
	kernel_check_socket_sizes(ctx->i_puc);
	return ret;
}

static int tool_input_read(struct tool_input_context_s *ctx) {
	ctx->bytes_available = 0;

	int rlen = avio_read(ctx->i_puc, ctx->buf, ctx->buflen);

	if (ctx->verbose & 1) {
		printf("source %s received %d bytes (EAGAIN %d ETIMEDOUT %d)\n", ctx->iname, rlen, -EAGAIN, -ETIMEDOUT);
	}

	if (rlen < -1) {
		errno = -rlen;
		return (errno == EAGAIN || errno == ETIMEDOUT) ? 0 : rlen;
	}
	else if (rlen >= 0) {
		ctx->bytes_available = rlen;
	}

	return rlen;
}

static unsigned char * tool_input_seek_pat(struct tool_input_context_s *ctx, int *bytesRemaining)
{
	if (ctx->buf) {
		for (unsigned char *p = ctx->buf; p < ctx->buf + ctx->bytes_available; p += TS_SIZE) {	
			// strip the RTP header if appropriate	
			if (rtp_check_hdr(p) && rtp_get_type(p) == 33) {
				p = rtp_payload(p);
			}

			if (!ts_validate(p)) continue;

			if (ts_get_pid(p) == 0) {
				*bytesRemaining =  ctx->bytes_available - (p - ctx->buf);
				return p;
			}
		}
	}


	*bytesRemaining = 0;
	return NULL;
}

#define MAX_INPUTS 10

struct tool_context_s
{
	int input_count;
	struct tool_input_context_s *inputs[10];
	char *oname;
	int verbose;
	int skipSmoother;
	int stopAfterSeconds;
	int terminateLOSSeconds;
	int showDeveloperStatisticsSeconds;

	struct ltntstools_stream_statistics_s *o_stream;
	void *smoother;

	pthread_t threadId;
	int trailerRow;
	int threadTerminate, threadRunning, threadTerminated;


	int active_input;
	atomic_int requested_input;

#ifdef __linux__
	timer_t timerId;
#endif

	/* ffmpeg related */
	pthread_t ffmpeg_threadId;
	atomic_int ffmpeg_threadTerminate, ffmpeg_threadRunning;
	AVIOContext *o_puc;	

	int latencyMS;
	int pcrPID;    /* UDP-TS only */
	int isRTP;     /* Boolean. True = RTP mode, false = UDP-TS PCR mode */

	/* The reframer used only in UDP-TS mode, NOT in RTP mode, to create 7*188 packet lengths. */
	struct ltntstools_reframer_ctx_s *reframer;

	struct rtp_hdr_analyzer_s rtp_stream_out;

	unsigned int pid;

};

/* Reframer hands us 7*188 buffers, guaranteed. Send to the UDP. */
static void *reframer_cb(void *userContext, const uint8_t *buf, int lengthBytes)
{
	struct tool_context_s *ctx = userContext;
	avio_write(ctx->o_puc, buf, lengthBytes);
	return NULL;
}

static int smoother_pcr_cb(void *userContext, unsigned char *buf, int byteCount,
	struct ltntstools_pcr_position_s *array, int arrayLength)
{
	struct tool_context_s *ctx = userContext;

	if (ctx->verbose & 8) { /* Output hex dump */
		for (int i = 0; i < arrayLength; i++) {
			struct ltntstools_pcr_position_s *e = &array[i];
			char *ts = NULL;
			ltntstools_pcr_to_ascii(&ts, e->pcr);
			printf("%s : %14" PRIi64 ", %8" PRIu64 ", %04x\n",
				ts,
				e->pcr, e->offset, e->pid);
			free(ts);
		}
	}
	for (int i = 0; i < byteCount; i += 188) {
		uint16_t pidnr = ltntstools_pid(buf + i);
		struct ltntstools_pid_statistics_s *pid = &ctx->o_stream->pids[pidnr];

		pid->enabled = 1;
		pid->packetCount++;

		uint8_t cc = ltntstools_continuity_counter(buf + i);
		if (ltntstools_isCCInError(buf + i, pid->lastCC)) {
			if (pid->packetCount > 1 && pidnr != 0x1fff) {
				char ts[256];
				time_t now = time(0);
				sprintf(ts, "%s", ctime(&now));
				ts[ strlen(ts) - 1] = 0;
				printf("%s: %s() CC Error : index=%04d, pid %04x -- Got 0x%x wanted 0x%x\n", ts, __func__, i, pidnr, cc, (pid->lastCC + 1) & 0x0f);
				printf("scb %02x %02x %02x %02x %02x %02x %02x %02x\n",
					*(buf + i + 0),
					*(buf + i + 1),
					*(buf + i + 2),
					*(buf + i + 3),
					*(buf + i + 4),
					*(buf + i + 5),
					*(buf + i + 6),
					*(buf + i + 7)
				);
				pid->ccErrors++;
			}
		}
		// else {
		// 	printf("%s() CC OK : index=%04d, pid %04x -- Got 0x%x wanted 0x%x\n", __func__, i, pidnr, cc, (pid->lastCC + 1) & 0x0f);
		// }

		pid->lastCC = cc;

		if (ltntstools_tei_set(buf + i))
			pid->teiErrors++;

		if (ctx->verbose & 2) {
			for (int i = 0; i < byteCount; i += 188) {
				for (int j = 0; j < 24; j++) {
					printf("%02x ", buf[i + j]);
					if (j == 3)
						printf("-- 0x%04x(%4d) -- ", pidnr, pidnr);
				}
				printf("\n");
			}
		}
	}

	ltststools_reframer_write(ctx->reframer, buf, byteCount);

	return 0;
}

static void print_rtp_buffer(struct tool_context_s *ctx, const unsigned char *buf, int byteCount)
{
	struct rtp_hdr *hdr = (struct rtp_hdr *)buf;
	int tspktcount = ((byteCount - 12) / 188);

	char *ts = NULL;
	ltntstools_pts_to_ascii(&ts, ntohl(hdr->ts));
	printf("@ %s : %14" PRIu32 " -- RTP: [ ", ts, ntohl(hdr->ts));
	free(ts);

	/* Hexdump of the RTP header */
	for (int j = 0; j < 12; j++) {
		printf("%02x ", buf[j]);
	}
	printf("] = %d bytes, seq %d\n", byteCount, ntohs(hdr->seq));

	/* Hexdump, beginning of each TS packet */
	for (int i = 0; i < tspktcount; i++) {
		printf("  -> ");
		for (int j = 0; j < 12; j++) {
			printf("%02x ", buf[12 + (i * 188) + j]);
		}
		printf("\n");
	}
}

static int tool_write(struct tool_context_s *ctx, struct tool_input_context_s *ictx, unsigned char *buf, int byteCount)
{
	unsigned char *end = buf + byteCount;
	
	int i = 0;
	// printf("%s(): bytes=%d, packets=%d\n", __func__, byteCount, byteCount/TS_SIZE);
	for (unsigned char *p = buf; p < end; p += TS_SIZE, ++i) {	

		// strip the RTP header if appropriate	
		if (!ts_validate(p) && rtp_check_hdr(p) && rtp_get_type(p) == 33) {
			/* Monitor for RTP sequence problems. Assumes the buffer starts with the RTP header. */
			rtp_hdr_write(&ictx->rtp_stream_in, (struct rtp_hdr *)p);

			/* Dump the packet hex */
			if (ctx->verbose & 8) {
				print_rtp_buffer(ctx, p, end - p);
			}
			p = rtp_payload(p);
		}

		if (!ts_validate(p)) {
			fprintf(stderr, "invalid TS packet, index=%d/%d!\n", i, byteCount/TS_SIZE);
			continue;
		}
		else if (p + TS_SIZE > end) {
			fprintf(stderr, "not enough bytes to read packet, index=%d/%d!\n", i, byteCount/TS_SIZE);
			break;
		}

		
		ltntstools_pid_stats_update(ictx->i_stream, p, TS_SIZE);

		struct timeval now;
		gettimeofday(&now, NULL);
		// printf("%s(): index=%d, bytes=%d, packets=%d\n", __func__, i, byteCount, byteCount/TS_SIZE);
		if (ctx->skipSmoother || !ctx->smoother) {
			smoother_pcr_cb(ctx, p, TS_SIZE, NULL, 0);
		} else {
			smoother_pcr_write(ctx->smoother, p, TS_SIZE, &now);
		}
	}

	return 0;
}

static int tool_process_input(struct tool_context_s *ctx, int index) {
	if (index != ctx->active_input && index != ctx->requested_input) return 0;

	if (index == ctx->active_input) {
		struct tool_input_context_s *ictx = ctx->inputs[index];
		return tool_write(ctx, ictx, ictx->buf, ictx->bytes_available);
	}
	else if (ctx->requested_input != ctx->active_input && index == ctx->requested_input) {
		int len = 0;
		struct tool_input_context_s *ictx = ctx->inputs[index];
		unsigned char *data = tool_input_seek_pat(ictx, &len);
		if (data) {
			ctx->active_input = ctx->requested_input;
			return tool_write(ctx, ictx, data, len);
		}
	}
	return 0;
}

static void *thread_packet_rx(void *p)
{
	struct tool_context_s *ctx = p;
	ctx->ffmpeg_threadRunning = 1;
	ctx->ffmpeg_threadTerminate = 0;

	time_t lastPacketTime = time(0);
	time_t lastDevStatsTime = lastPacketTime;

	char ts[256];
	time_t now = time(0);
	time_t bannerPrint = 0;
	sprintf(ts, "%s", ctime(&now));
	ts[ strlen(ts) - 1] = 0;
	printf("%s: Smoother starting\n", ts);

	while (!ctx->ffmpeg_threadTerminate) {
		if (ctx->smoother && ctx->showDeveloperStatisticsSeconds && (lastDevStatsTime + ctx->showDeveloperStatisticsSeconds <= now)) {
			lastDevStatsTime = now;

			char ts[256];
			time_t now = time(0);
			sprintf(ts, "%s", ctime(&now));
			ts[ strlen(ts) - 1] = 0;

			struct smoother_pcr_statistics s;
			smoother_pcr_get_statistics(ctx->smoother, &s);

			printf("%s: Dev Statistics - max observed latency %6" PRIi64 "(ms) alloc %8" PRIi64
				"(B) used %8" PRIu64 "(B) items %5" PRIu64" free %5" PRIu64 " busy %5" PRIu64 " growth %5" PRIu64 " \n",
				ts,
				s.measuredLatencyMs_hwm,
				s.totalAllocFootprintBytes,
				s.totalUserBytes,
				s.totalItems,
				s.qFreeCount,
				s.qBusyCount,
				s.totalItemGrowth);
		}

		if (ctx->terminateLOSSeconds && (lastPacketTime + ctx->terminateLOSSeconds <= now)) {
			char ts[256];
			time_t now = time(0);
			sprintf(ts, "%s", ctime(&now));
			ts[ strlen(ts) - 1] = 0;

			/* We lost input packets for N seconds. Terminate cleanly. */
			printf("%s: LOS occured for %d seconds. Terminating at %s",
				ts,
				ctx->terminateLOSSeconds,
				ctime(&now));
			exit(1);
		}

		bool ok = true;
		int bytes_read = 0;
		for (int i = 0; i < ctx->input_count; ++i) {
			int rlen = tool_input_read(ctx->inputs[i]);
			if (rlen > 0) {
				bytes_read += rlen;
				if (tool_process_input(ctx, i) < 0) {
					ok = false;
					break;
				}
			}
			else if (rlen < 0) {
				printf("error occurred with stream %d, exiting.\n", i);
				ok = false;
				break;
			}
		}

		if (!ok) break;

		if (!bytes_read) {
			usleep(1000);  // save some cycles, easier than switching to poll().
		}

		now = time(0);
		if (bannerPrint + (60 * 60 * 24) < now) {
			bannerPrint = now;
			printToolBanner("tstools_bitrate_smoother", GIT_VERSION);
		}

		lastPacketTime = now;

	}

	pthread_exit(0);
	return 0;
}


#ifdef __linux__
static void timer_thread(union sigval arg)
{
	signal_handler(0);
}

static void terminate_after_seconds(struct tool_context_s *ctx, int seconds)
{
	struct sigevent se;
	se.sigev_notify = SIGEV_THREAD;
	se.sigev_value.sival_ptr = &ctx->timerId;
	se.sigev_notify_function = timer_thread;
	se.sigev_notify_attributes = NULL;

	struct itimerspec ts;
	ts.it_value.tv_sec = seconds;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;

	int ret = timer_create(CLOCK_REALTIME, &se, &ctx->timerId);
	if (ret < 0) {
		fprintf(stderr, "Failed to create termination timer.\n");
		return;
	}

	ret = timer_settime(ctx->timerId, 0, &ts, 0);
	if (ret < 0) {
		fprintf(stderr, "Failed to start termination timer.\n");
		return;
	}
}
#endif

static void usage(const char *progname)
{
	printf("A tool to smooth input RTP-TS and UDP-TS CBR bitrate MPEG-TS streams.\n");
	printf("Usage:\n");
	printf("  -i <url> Eg: udp|rtp://234.1.1.1:4160?localaddr=172.16.0.67\n");
	printf("           172.16.0.67 is the IP addr where we'll issue an IGMP join\n");
	printf("  -o <url> Eg: udp|rtp://234.1.1.1:4560\n");
	printf("  -P 0xnnnn PID containing the PCR (UDP-TS Only. Optional)\n");
	printf("  -v # bitmask. Set level of verbosity. [def: 0]\n");
	printf("     1 - input packet hex\n");
	printf("     2 - output packet hex\n");
	printf("     4 - output packet PCR data and human readable PCR clock\n");
	printf("     8 - input packet RTP data and human readable clock\n");
	printf("    32 - PMT re-writing and PID removal\n");
	printf("  -R pid 0xNNNN to be removed [def: none], multiple -R instances supported. [0x2000 all pids]\n");
	printf("  -S <seconds> Show developer statistics every N seconds. [def: disabled]\n");
	printf("  -Z pid 0xNNNN Update this PID PMT to reflect any removed ES pids [def: disabled]\n");
	printf("  -l latency (ms) of protection. [def: %d]\n", DEFAULT_LATENCY);
#ifdef __linux__
	printf("  -t <#seconds> Stop after N seconds [def: 0 - unlimited]\n");
#endif
	printf("  -L <#seconds> During input LOS, terminate software after time. [def: 0 - don't terminate]\n");
	printf("  -X Skip the smoother and write the input packet direct to output as fast as possible (with reframing)\n");
	printf("  -h Display command line help.\n");
	printf("\n  Example UDP or RTP, don't mix'n'match:\n");
	printf("    tstools_input_selector -i 'udp://227.1.20.80:4002?localaddr=192.168.20.45&buffer_size=250000' \\\n");
	printf("      -o udp://227.1.20.45:4501?pkt_size=1316 -l 500\n");
	printf("\n  tstools_input_selector -i 'rtp://227.1.20.80:4002?localaddr=192.168.20.45&buffer_size=250000' \\\n");
	printf("      -o rtp://227.1.20.45:4501?pkt_size=1328 -l 500\n");
}

static int read_stdin(int *input)
{
	char *line = NULL;
	size_t line_size;
	ssize_t nread;
	if ((nread = getline(&line, &line_size, stdin)) == -1) {
		free(line);
		return -1;
	}

	char *p = line, *endptr;			
	while (isspace(*p)) ++p, --line_size;
	if (line_size && *p) {
		errno = 0;
		int v = strtol(p, &endptr, 0);
		if ((errno == ERANGE && (v == LONG_MAX || v == LONG_MIN)) || (errno != 0 && v == 0)) {
			perror("strtol");
		}

		else if (endptr == p) {
			fprintf(stderr, "No digits were found\n");
		}
		else {
			*input = v;
		}
	}

	free(line);
	return 0;
}

int input_selector(int argc, char *argv[])
{
	int ret = 0;
	int ch;

	struct tool_context_s tctx, *ctx;
	ctx = &tctx;
	memset(ctx, 0, sizeof(*ctx));

	ctx->latencyMS = DEFAULT_LATENCY;
	ctx->reframer = ltntstools_reframer_alloc(ctx, 7 * 188, (ltntstools_reframer_callback)reframer_cb);

	ltntstools_pid_stats_alloc(&ctx->o_stream);

	while ((ch = getopt(argc, argv, "?hi:l:o:L:P:R:v:t:S:XZ:")) != -1) {
		switch (ch) {
		case '?':
		case 'h':
			usage(argv[0]);
			exit(1);
			break;
		case 'l':
			ctx->latencyMS = atoi(optarg);
			break;
		case 'i':
			if (ctx->input_count >= MAX_INPUTS) {
				fprintf(stderr, "too many inputs! max=%d\n", MAX_INPUTS);
				exit(1);
			}
			if (!(ctx->inputs[ctx->input_count] = tool_input_new(optarg))) {
				fprintf(stderr, "Failed to allocate input!\n");
				exit(1);
			}
			++ctx->input_count;
			break;
		case 'v':
			ctx->verbose = atoi(optarg);
			break;
		case 'o':
			ctx->oname = optarg;
			break;
		case 'L':
			ctx->terminateLOSSeconds = atoi(optarg);
			break;
		case 'P':
			if ((sscanf(optarg, "0x%x", &ctx->pcrPID) != 1) || (ctx->pcrPID > 0x1fff)) {
					usage(argv[0]);
					exit(1);
			}
			break;
		case 'R':
			if ((sscanf(optarg, "0x%x", &ctx->pid) != 1) || (ctx->pid > 0x2000)) {
				usage(argv[0]);
				exit(1);
			}
			break;
		case 'S':
			ctx->showDeveloperStatisticsSeconds = atoi(optarg);
			if (ctx->showDeveloperStatisticsSeconds < 5) {
				ctx->showDeveloperStatisticsSeconds = 5;
			}
			break;
#ifdef __linux__
		case 't':
			ctx->stopAfterSeconds = atoi(optarg);
			break;
#endif
		case 'X':
			ctx->skipSmoother = 1;
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (ctx->input_count == 0) {
		usage(argv[0]);
		fprintf(stderr, "\n-i is mandatory, aborting.\n\n");
		exit(1);
	}

	if (ctx->oname == NULL) {
		usage(argv[0]);
		fprintf(stderr, "\n-o is mandatory, aborting.\n\n");
		exit(1);
	}

	if (ctx->terminateLOSSeconds) {
		printf("\n-L %d, process will self terminate if input LOS exceeds %d seconds.\n\n", ctx->terminateLOSSeconds, ctx->terminateLOSSeconds);
	}

	if (ctx->stopAfterSeconds) {
#ifdef __linux__
		terminate_after_seconds(ctx, ctx->stopAfterSeconds);
#endif
	}

	if (strcasestr(ctx->oname, "rtp:")) {
		ctx->isRTP = 1;
		rtp_analyzer_init(&ctx->rtp_stream_out);
	}

	avformat_network_init();

	for (int i = 0; i < ctx->input_count; ++i) {
		ret = tool_input_open(ctx->inputs[i]);
		if (ret < 0) {
			fprintf(stderr, "-i syntax error, index=%d\n", i);
			goto no_output;
		}
	}


	ret = avio_open2(&ctx->o_puc, ctx->oname, AVIO_FLAG_WRITE | AVIO_FLAG_NONBLOCK | AVIO_FLAG_DIRECT, NULL, NULL);
	if (ret < 0) {
		fprintf(stderr, "-o syntax error\n");
		ret = -1;
		goto no_output;
	}

	/* Preallocate enough throughput measures for approx a 40mbit stream */
	signal(SIGINT, signal_handler);
	gRunning = 1;

	pthread_create(&ctx->ffmpeg_threadId, 0, thread_packet_rx, ctx);
	
	while (gRunning) {
		int new_input = -1;
		if (read_stdin(&new_input) == -1) break;
		if (new_input >= 0) {
			ctx->requested_input = new_input;
			printf("Requesting switch to input %d", new_input);
		}		
	}

	/* Shutdown ffmpeg */
	gRunning = false;
	ctx->ffmpeg_threadTerminate = 1;
	pthread_join(ctx->ffmpeg_threadId, NULL);

	avio_close(ctx->o_puc);

	if (ctx->isRTP == 0) {
		if (ctx->smoother) {
			smoother_pcr_free(ctx->smoother);
			ctx->smoother = 0;
		}
	} else {
		if (ctx->smoother) {
			smoother_rtp_free(ctx->smoother);
			ctx->smoother = 0;
		}
	}
	ctx->smoother = 0;

	ltntstools_reframer_free(ctx->reframer);

	ret = 0;

	for (int i = 0; i < ctx->input_count; ++i)
		printf("\nInput %d from %s\n", i, ctx->inputs[i]->iname);
	if (ctx->oname)
		printf("Output to %s\n", ctx->oname);

	int64_t errCount = 0;

	for (int v = 0; v < ctx->input_count; ++v) {
		struct tool_input_context_s *ictx = ctx->inputs[v];
		if (strcasestr(ictx->iname, "rtp:")) {
			rtp_analyzer_report_dprintf(&ictx->rtp_stream_in, STDOUT_FILENO);
		}

		printf("\nI: PID   PID     PacketCount   CCErrors  TEIErrors\n");
		printf("----------------------------  --------- ----------\n");
		for (int i = 0; i < MAX_PID; i++) {
			struct ltntstools_stream_statistics_s * stats = ictx->i_stream;
			if (stats->pids[i].enabled) {
				printf("0x%04x (%4d) %14" PRIu64 " %10" PRIu64 " %10" PRIu64 "\n", i, i,
					stats->pids[i].packetCount,
					stats->pids[i].ccErrors,
					stats->pids[i].teiErrors);
				errCount += stats->pids[i].ccErrors;
			}
		}
	}

	if (ctx->isRTP) {
		rtp_analyzer_report_dprintf(&ctx->rtp_stream_out, STDOUT_FILENO);
	}

	printf("O: PID   PID     PacketCount   CCErrors  TEIErrors\n");
	printf("----------------------------  --------- ----------\n");
	for (int i = 0; i < MAX_PID; i++) {
		if (ctx->o_stream->pids[i].enabled) {
			printf("0x%04x (%4d) %14" PRIu64 " %10" PRIu64 " %10" PRIu64 "\n", i, i,
				ctx->o_stream->pids[i].packetCount,
				ctx->o_stream->pids[i].ccErrors,
				ctx->o_stream->pids[i].teiErrors);
			errCount += ctx->o_stream->pids[i].ccErrors;
		}
	}

	ltntstools_pid_stats_free(ctx->o_stream);

	if (ctx->isRTP) {
		rtp_analyzer_free(&ctx->rtp_stream_out);
	}

	ret = 0;

no_output:
	return ret;
}
