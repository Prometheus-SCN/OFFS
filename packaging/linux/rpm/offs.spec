Name:           offs
Version:        0.1.0
Release:        1%{?dist}
Summary:        Owner Free File System daemon and CLI
License:        MIT
URL:            https://github.com/Prometheus-SCN/OFFS
Source0:        %{name}-%{version}.tar.gz

Requires:       openssl >= 3.0
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

%description
OFFS is a decentralized, content-addressed storage system.
This package installs the offs-daemon system service and offs CLI tool.

%prep
%setup -q

%install
install -Dm755 offs-daemon %{buildroot}/usr/bin/offs-daemon
install -Dm755 offs-cli %{buildroot}/usr/bin/offs
install -Dm755 offs-updater %{buildroot}/usr/bin/offs-updater
install -Dm644 packaging/linux/rpm/offs-daemon.service %{buildroot}/usr/lib/systemd/system/offs-daemon.service

%post
%systemd_post offs-daemon.service

%preun
%systemd_preun offs-daemon.service

%postun
%systemd_postun_with_restart offs-daemon.service

%files
/usr/bin/offs-daemon
/usr/bin/offs
/usr/bin/offs-updater
/usr/lib/systemd/system/offs-daemon.service
