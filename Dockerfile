FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl g++ make unzip \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY scripts/fetch-arrow scripts/fetch-arrow
RUN sh ./scripts/fetch-arrow /opt/arrow
COPY Makefile main.cpp input.hpp output.hpp ./
RUN make ARROW_DIR=/opt/arrow

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/arrow/libarrow.so.2200 /opt/arrow/libparquet.so.2200 /opt/arrow/
COPY --from=build /opt/arrow/licenses /usr/share/doc/shopping-sessions/arrow/
COPY --from=build /build/shopping-sessions /usr/local/bin/shopping-sessions
ENV DATA_DIR=/data OUTPUT_DIR=/output NCPU=8
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/shopping-sessions"]
