FROM postgres:11

MAINTAINER yamatattsu@gmail.com

RUN apt-get update && apt-get install -y \
git \
build-essential \
bc \
libpam-dev \
libedit-dev \
libkrb5-dev \
libssl-dev \
libselinux-dev \
libpam-dev \
libkrb5-dev \
zlib1g-dev \
libedit-dev \
wget \
vim \
postgresql-server-dev-11 \
postgresql-client-11 \
postgresql-contrib-11 \
libpq-dev 

# Workaround. See:https://github.com/zalando/spilo/pull/229/commits/dfd74c94e060f9b8b5c080d66846657aaa21bae8
RUN ln -snf /usr/lib/postgresql/${version}/lib/libpgcommon.a /usr/lib/x86_64-linux-gnu/libpgcommon.a && \
ln -snf /usr/lib/postgresql/${version}/lib/libpgport.a /usr/lib/x86_64-linux-gnu/libpgport.a 

# Get extensions
RUN git clone https://github.com/ossc-db/pg_hint_plan.git && \
wget https://github.com/ossc-db/pg_store_plans/archive/1.3.tar.gz && \
git clone https://github.com/ossc-db/pg_plan_advsr.git pg_plan_advsr

# Copy files for pg_plan_advsr
RUN cd pg_hint_plan && git checkout PG11 && cp pg_stat_statements.c ../pg_plan_advsr/ && \
cp normalize_query.h ../pg_plan_advsr/ && \
cd .. && tar xvzf 1.3.tar.gz && cp pg_store_plans-1.3/pgsp_json*.[ch] pg_plan_advsr/

# Build & install
RUN cd pg_hint_plan && make -s && make -s install && \
export PATH=$PATH:/usr/lib/postgresql/11/bin && cd ../pg_store_plans-1.3 && make USE_PGXS=1 all install && \
cd ../pg_plan_advsr && make -s && make -s install

# Create extensons
## you can use create_extensions.sh by postgres user manualy
COPY ./create_extensions.sh /docker-entrypoint-initdb.d/


