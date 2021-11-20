ARG PG_SERVER_VERSION=14

FROM ghcr.io/covid-genomics/bioseq-postgres-dev:latest

WORKDIR /source

ADD ./bioseq_lib ./bioseq_lib
ADD ./bioseq_pg ./bioseq_pg
ADD ./sql ./sql
ADD ./CMakeLists.txt ./CMakeLists.txt
ADD ./bioseq_pg/bioseq.control ./bioseq.control
ADD ./scripts/pg_local_build.sh ./build.sh

RUN rm -rfd build && ./build.sh -DBUILD_SHARED_LIBS=true

RUN cd / && rm -rf /tmp/* && apt-get purge -y --auto-remove gcc \
    make wget unzip curl libc6-dev apt-transport-https git \
    postgresql-server-dev-${PG_SERVER_VERSION} pgxnclient build-essential \
    libssl-dev krb5-multidev comerr-dev krb5-multidev libkrb5-dev apt-utils lsb-release \
    libgssrpc4 \
    && apt-get clean -y autoclean \
    && rm -rf /var/lib/apt/lists/* \
    # remove standard pgdata
    && rm -rf /var/lib/postgresql/${PG_SERVER_VERSION}/

EXPOSE 5432

# Prepare Postgres start script
RUN echo "#!/bin/bash" > /pg_start.sh && chmod a+x /pg_start.sh \
    && echo "chown -R postgres:postgres \${PGDATA} /var/run/postgresql" \
    >> /pg_start.sh \
    && printf "sudo -Eu postgres /usr/lib/postgresql/${PG_SERVER_VERSION}/bin/postgres -D \${PGDATA} >& /proc/1/fd/1 \n" \
    >> /pg_start.sh \
    # Infinite sleep to allow restarting Postgres
    && echo "/bin/bash -c \"trap : TERM INT; sleep infinity & wait\"" \
    >> /pg_start.sh

# make the "en_US.UTF-8" locale so postgres will be utf-8 enabled by default
RUN apt-get install -y locales; rm -rf /var/lib/apt/lists/*; \
    localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8
ENV LANG en_US.utf8

ENV PGDATA /var/lib/postgresql/data
RUN mkdir -p "$PGDATA"
VOLUME /var/lib/postgresql/data

ADD ./scripts/pg_start.sh /source/pg_start.sh
RUN mkdir -p "$PGDATA" && chown -R postgres:postgres "$PGDATA" && chmod 777 "$PGDATA"
RUN chown -R postgres:postgres /source/pg_start.sh && chmod u+x /source/pg_start.sh

# Perform smoke test
ARG RUN_SMOKE_TEST="false"
ADD ./scripts/pg_run_smoke_test.sh /source/pg_run_smoke_test.sh
ADD ./scripts/smoke_test.sql /source/smoke_test.sql
RUN /source/pg_run_smoke_test.sh /source/smoke_test.sql

# Remove smoke test files
RUN rm -f /source/pg_run_smoke_test.sh /source/smoke_test.sql

USER postgres
ENTRYPOINT ["/source/pg_start.sh"]

EXPOSE 5432
CMD ["postgres"]