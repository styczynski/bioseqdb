ARG PG_SERVER_VERSION=14

FROM postgres:${PG_SERVER_VERSION}
LABEL maintainer="postgres.ai"

ARG PG_SERVER_VERSION
ENV PG_SERVER_VERSION=${PG_SERVER_VERSION:-14}

ARG WALG_VERSION
ENV WALG_VERSION=${WALG_VERSION:-0.2.19}

RUN apt-get clean && rm -rf /var/lib/apt/lists/partial \
    && apt-get update -o Acquire::CompressionTypes::Order::=gz \
    && apt-get install --no-install-recommends -y libssl-dev wget make gcc unzip sudo git \
    curl libc6-dev apt-transport-https ca-certificates pgxnclient bc \
    build-essential libevent-dev libssl-dev krb5-multidev libkrb5-dev lsb-release apt-utils \
    && apt-get install --no-install-recommends -y \
    postgresql-server-dev-${PG_SERVER_VERSION} \
    && apt-get install --no-install-recommends -y postgresql-${PG_SERVER_VERSION}-repack \
    && apt-get install --no-install-recommends -y \
    postgresql-plpython3-${PG_SERVER_VERSION} \
    # hypopg
    && apt-get install --no-install-recommends -y \
    postgresql-${PG_SERVER_VERSION}-hypopg \
    && apt-get install --no-install-recommends -y \
    postgresql-${PG_SERVER_VERSION}-hypopg-dbgsym \
    # pgaudit extension
    && apt-get install --no-install-recommends -y postgresql-${PG_SERVER_VERSION}-pgaudit \
    && if [ $(echo "$PG_SERVER_VERSION < 14" | /usr/bin/bc) = "1" ]; then \
    # pg_hint_plan extension (dots are to be skipped here, e.g., "9.6" -> "96")
    export PG_PLAN_HINT_VERSION=$(echo $PG_SERVER_VERSION | sed 's/\.//') \
    && wget --quiet -O /tmp/pg_hint_plan.zip \
    https://github.com/ossc-db/pg_hint_plan/archive/PG${PG_PLAN_HINT_VERSION}.zip \
    && unzip /tmp/pg_hint_plan.zip -d /tmp \
    && cd /tmp/pg_hint_plan-PG${PG_PLAN_HINT_VERSION} \
    && make && make install; \
    fi \
    # extensions supported in PostgreSQL 12 and below
    # bc is used to manage PostgreSQL versions with dot like 9.6
    && if [ $(echo "$PG_SERVER_VERSION < 13" | /usr/bin/bc) = "1" ]; then \
    # powa extension
    apt-get install postgresql-${PG_SERVER_VERSION}-powa \
    # pg_auth_mon extension
    && git clone https://github.com/RafiaSabih/pg_auth_mon.git \
    && cd pg_auth_mon && USE_PGXS=1 make && USE_PGXS=1 make install; \
    fi \
    # timescaledb extension
    && if [ $(echo "$PG_SERVER_VERSION < 11" | /usr/bin/bc) = "1" ]; then \
    echo 'deb https://packagecloud.io/timescale/timescaledb/debian/' \
    $(env -i bash -c '. /etc/os-release; echo ${VERSION_CODENAME}') \
    'main' > /etc/apt/sources.list.d/timescaledb.list \
    && wget --quiet -O - https://packagecloud.io/timescale/timescaledb/gpgkey | sudo apt-key add - \
    && apt-get update \
    && apt-get install -y timescaledb-postgresql-${PG_SERVER_VERSION}; \
    elif [ $(echo "$PG_SERVER_VERSION < 14" | /usr/bin/bc) = "1" ]; then \
    echo "deb https://packagecloud.io/timescale/timescaledb/debian/ $(lsb_release -c -s) main" > /etc/apt/sources.list.d/timescaledb.list \
    && wget --quiet -O - https://packagecloud.io/timescale/timescaledb/gpgkey | sudo apt-key add - \
    && apt-get update \
    && apt install -y timescaledb-2-postgresql-${PG_SERVER_VERSION}; \
    fi \
    # citus extension; only versions Postgres 11+ are supported
    && if [ "${PG_SERVER_VERSION}" = "11" ]; then \
    curl -s https://install.citusdata.com/community/deb.sh | bash \
    && apt-get install -y postgresql-${PG_SERVER_VERSION}-citus-9.4 \
    postgresql-${PG_SERVER_VERSION}-hll=2.14.citus-1 \
    postgresql-${PG_SERVER_VERSION}-topn=2.3.0; \
    fi \
    && if [ $(echo "$PG_SERVER_VERSION > 11" | /usr/bin/bc) = "1" ]; then \
    curl -s https://install.citusdata.com/community/deb.sh | bash \
    && apt-get install -y postgresql-${PG_SERVER_VERSION}-citus-10.2 \
    postgresql-${PG_SERVER_VERSION}-hll=2.16.citus-1 \
    postgresql-${PG_SERVER_VERSION}-topn=2.4.0; \
    fi \
    # pg_timetable extension
    && wget https://github.com/cybertec-postgresql/pg_timetable/releases/download/v2.3.0/pg_timetable_2.3.0_Linux_x86_64.deb \
    && dpkg -i pg_timetable_2.3.0_Linux_x86_64.deb \
    && rm -rf pg_timetable_2.3.0_Linux_x86_64.deb \
    # pg_show_plans extension
    && git clone https://github.com/cybertec-postgresql/pg_show_plans.git \
    && cd pg_show_plans \
    && export USE_PGXS=1 && make && make install && cd .. && rm -rf pg_show_plans \
    # pg_cron extension
    && apt-get install -y postgresql-${PG_SERVER_VERSION}-cron \
    # postgresql_anonymizer extension
    && pgxn install ddlx && pgxn install postgresql_anonymizer \
    # pg_stat_kcache extension
    && apt-get install postgresql-${PG_SERVER_VERSION}-pg-stat-kcache \
    # add pg_qualstats extension
    && apt-get install postgresql-${PG_SERVER_VERSION}-pg-qualstats \
    && if [ $(echo "$PG_SERVER_VERSION < 12" | /usr/bin/bc) = "1" ]; then \
    # bg_mon extension
    apt-get install -yq --no-install-suggests --no-install-recommends brotli \
    && git clone https://github.com/CyberDem0n/bg_mon.git && cd bg_mon \
    && USE_PGXS=1 make && USE_PGXS=1 make install && cd .. ; \
    fi \
    # pgextwlist extension
    && apt-get install postgresql-${PG_SERVER_VERSION}-pgextwlist \
    # set_user extension
    && git clone https://github.com/pgaudit/set_user.git \
    && cd set_user && git checkout REL3_0_0 && make USE_PGXS=1 && make USE_PGXS=1 install \
    # errorlogs extension
    && if [ $(echo "$PG_SERVER_VERSION > 9.6" | /usr/bin/bc) = "1" ]; then \
    cd /tmp && wget https://github.com/munakoiso/logerrors/archive/v2.0.tar.gz \
    && tar -xf v2.0.tar.gz && rm v2.0.tar.gz && cd logerrors-2.0 \
    && USE_PGXS=1 make && USE_PGXS=1 make install; \
    fi \
    # WAL-G
    && wget --quiet -O /tmp/wal-g.linux-amd64.tar.gz "https://github.com/wal-g/wal-g/releases/download/v${WALG_VERSION}/wal-g.linux-amd64.tar.gz" \
    && tar -zxvf /tmp/wal-g.linux-amd64.tar.gz && mv wal-g /usr/local/bin/


RUN apt-get update -y
RUN echo "deb http://deb.debian.org/debian experimental main" >> /etc/apt/sources.list \
    && echo "deb http://ftp.us.debian.org/debian testing main contrib non-free" > /etc/apt/sources.list.d/testing.list \
    && printf "Package: *\nPin: release a=testing\nPin-Priority: 100" > /etc/apt/preferences.d/testing
RUN apt-get update -y
RUN apt-get install -t testing g++ -y -o APT::Immediate-Configure=0

RUN wget https://cmake.org/files/v3.17/cmake-3.17.1.tar.gz
RUN tar -xzvf cmake-3.17.1.tar.gz

RUN cd cmake-3.17.1/ && ./bootstrap --prefix=/usr && make && make install

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