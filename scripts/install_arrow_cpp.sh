#!/usr/bin/env bash
# Install Apache Arrow C++ development packages for cyphal-reassemble builds.
set -euo pipefail

if [[ -f /etc/debian_version ]]; then
  apt-get update
  apt-get install -y -V ca-certificates lsb-release wget cmake g++ make git
  CODENAME="$(lsb_release --codename --short)"
  ID_SHORT="$(lsb_release --id --short | tr 'A-Z' 'a-z')"
  wget "https://apache.jfrog.io/artifactory/arrow/${ID_SHORT}/apache-arrow-apt-source-latest-${CODENAME}.deb"
  apt-get install -y -V "./apache-arrow-apt-source-latest-${CODENAME}.deb"
  apt-get update
  apt-get install -y -V libarrow-dev patchelf
elif [[ -f /etc/redhat-release ]] || [[ -f /etc/almalinux-release ]]; then
  yum install -y wget cmake gcc-c++ make git
  ARCH="$(uname -m)"
  RELEASE_RPM="https://apache.jfrog.io/artifactory/arrow/almalinux/8/apache-arrow-release-latest.rpm"
  wget -q "${RELEASE_RPM}" -O /tmp/apache-arrow-release.rpm
  yum install -y /tmp/apache-arrow-release.rpm
  yum install -y arrow-devel patchelf
else
  echo "Unsupported OS for Arrow C++ install: $(uname -a)" >&2
  exit 1
fi

echo "==> Arrow C++ installed"
