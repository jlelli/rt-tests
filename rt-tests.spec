Summary: Programs that test various rt-features
Name: rt-tests
Version: 0.18
Release: 1
License: GPLv2
Group: Development/Tools
URL: git://git.kernel.org/pub/scm/linux/kernel/git/tglx/rt-tests.git
Source0: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root

%description
A set of programs that test and measure various components of "realtime"
kernel behavior, such as timer latency, signal latency and the functioning
of priority-inheritance mutexes.

%prep
%setup -qn rt-tests

%build
make

%install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT/usr/bin
mkdir -p $RPM_BUILD_ROOT/usr/share/man/man8
make DESTDIR=$RPM_BUILD_ROOT/usr install

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
/usr/bin/classic_pi
/usr/bin/cyclictest
/usr/bin/pi_stress
/usr/bin/signaltest
%doc
/usr/share/man/man8/cyclictest.8.gz
/usr/share/man/man8/pi_stress.8.gz

%changelog
* Thu Jan 03 2008 Clark Williams <williams@redhat.com> - 0.18-1
- Initial build.

