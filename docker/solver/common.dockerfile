FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get -y update && apt-get install -y apt-utils && \
    apt-get install -y --no-install-recommends \
    build-essential \
    make \
    gcc \
    git \
    file \
    less \
    pkg-config \
    wget \
    curl \
    swig \
    netpbm \
    libnetpbm-dev \
    wcslib-dev \
    wcslib-tools \
    zlib1g-dev \
    libbz2-dev \
    libcairo2-dev \
    libcfitsio-dev \
    libcfitsio-bin \
    libgsl-dev \
    libjpeg-dev \
    libpng-dev \
    python3 \
    python3-dev \
    python3-pip \
    python3-setuptools \
    python3-wheel \
    python3-pil \
    python3-numpy \
    python3-scipy \
    python3-matplotlib \
    source-extractor \
    && apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*
#    python3-tk \
#    plplot-driver-cairo \
#    libplplot-dev \

#    numpy \
#    scipy \
#    matplotlib \
# Pip installs
# Pin astropy<7: astropy 8.x demands numpy>=2 and packaging>=25, which pip cannot
# install over the dpkg-managed distro numpy/packaging (no RECORD file) and which
# would break the apt-built scipy/matplotlib ABI. astropy 6.x is satisfied by the
# distro numpy 1.26 / packaging 24, so pip upgrades nothing.
RUN for x in \
    fitsio \
    'astropy<7' \
    ; do pip3 install --no-cache --break-system-packages $x; done

# to help astrometry.net find netpbm (yuck)
RUN ln -s /usr/include /usr/local/include/netpbm

# python = python3
RUN ln -s /usr/bin/python3 /usr/bin/python
# Add the directory where the astrometry.net code puts its python libraries to PYTHONPATH
ENV PYTHONPATH=/usr/local/lib/python

RUN mkdir /src
WORKDIR /src

RUN git config --global --add safe.directory /src/astrometry

RUN mkdir /usr/local/data && cd /usr/local/data \
    && for i in $(seq -w 7 19); do \
    wget -nv https://data.astrometry.net/4100/index-41$i.fits; done

# dev.dockerfile, release.dockerfile or test.dockerfile will be appended
# during the build process using the shell script.
