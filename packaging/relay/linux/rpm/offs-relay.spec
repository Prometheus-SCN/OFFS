Name:           offs-relay
Version:        0.1.0
Release:        1%{?dist}
Summary:        OFFS relay server
License:        MIT
URL:            https://github.com/Prometheus-SCN/OFFS
Source0:        %{name}-%{version}.tar.gz

Requires:       openssl >= 3.0

%description
A QUIC-based relay server for the Owner Free File System.
This package installs the offs_relay binary to /usr/bin.

%prep
%setup -q

%install
install -Dm755 offs_relay %{buildroot}/usr/bin/offs_relay

%files
/usr/bin/offs_relay
