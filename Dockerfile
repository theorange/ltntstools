FROM container.make.tv/3rd-party/library/almalinux:8

COPY rpm/*.rpm /

RUN yum install -y epel-release \
    && yum install -y /*.rpm \
    && rm -f /*.rpm \
    && yum clean all