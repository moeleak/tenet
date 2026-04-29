FROM debian:trixie-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential make ca-certificates libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make clean && make

FROM debian:trixie-slim

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        openssh-server ca-certificates passwd adduser libsqlite3-0 \
        sssd sssd-ldap libnss-sss libpam-sss ldap-utils \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /run/sshd /run/tenet /run/sssd /etc/tenet /etc/sssd /var/lib/tenet/ssh /usr/local/share/tenet

COPY --from=build /src/tenet /usr/local/bin/tenet
COPY --from=build /src/tenet-bot /usr/local/bin/tenet-bot
COPY assets/badapple.delta /usr/local/share/tenet/badapple.delta
COPY docker/entrypoint.sh /usr/local/bin/tenet-docker-entrypoint
COPY docker/tenet-ssh-command.sh /usr/local/bin/tenet-ssh-command

RUN chmod 0755 /usr/local/bin/tenet /usr/local/bin/tenet-bot /usr/local/bin/tenet-docker-entrypoint /usr/local/bin/tenet-ssh-command

EXPOSE 2222
ENTRYPOINT ["/usr/local/bin/tenet-docker-entrypoint"]
