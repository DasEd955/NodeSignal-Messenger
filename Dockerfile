FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libgtk-4-dev \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

# set working directory
WORKDIR /app

# copy project into container
COPY . .

# ensure assets exist in build output after build
RUN mkdir -p build && \
    cd build && \
    cmake .. && \
    make && \
    mkdir -p assets && \
    cp ../client/client.ui assets/ && \
    cp ../client/style.css assets/