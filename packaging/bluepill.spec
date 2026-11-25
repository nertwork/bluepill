Name:           bluepill
Version:        %{?_version:%{_version}}%{!?_version:1.2.16}
Release:        1
Summary:        Native Qt6/QML desktop & CLI client for iMessage, RCS, and SMS
License:        MIT
URL:            https://github.com/nertwork/bluepill
Source0:        bluepill-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qtsvg-devel
BuildRequires:  ImageMagick
BuildRequires:  ImageMagick-heic
BuildRequires:  libheif-tools

Requires:       qt6-qtbase
Requires:       qt6-qtbase-gui
Requires:       qt6-qtdeclarative
Requires:       qt6-qtsvg
Requires:       libnotify
Recommends:     libheif-tools
Recommends:     ImageMagick
Recommends:     libsecret
Recommends:     pipewire-utils

Provides:       imsg = %{version}-%{release}
Conflicts:      imsg

%description
BluePill is a native Qt6/QML desktop and headless CLI client for iMessage,
RCS, and SMS supporting the BluePill macOS Relay and BlueBubbles server APIs.

%prep
%autosetup -n bluepill-%{version}

%build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
%cmake_build

%check
QT_QPA_PLATFORM=offscreen %ctest --output-on-failure

%install
%cmake_install
ln -sf bluepill %{buildroot}%{_bindir}/imsg
install -Dm644 pkgbuild/bluepill.service %{buildroot}%{_userunitdir}/bluepill.service
install -Dm644 pkgbuild/bluepill.desktop %{buildroot}%{_datadir}/applications/bluepill.desktop
install -Dm644 pkgbuild/bluepill.svg %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/bluepill.svg
install -Dm644 assets/bluepill.png %{buildroot}%{_datadir}/pixmaps/bluepill.png
install -Dm644 resources/imessage-notify.wav %{buildroot}%{_datadir}/bluepill/imessage-notify.wav
install -Dm644 resources/imessage-notify.wav %{buildroot}%{_datadir}/imsg/imessage-notify.wav

%files
%license LICENSE
%doc README.md CHANGELOG.md
%{_bindir}/bluepill
%{_bindir}/imsg
%{_userunitdir}/bluepill.service
%{_datadir}/applications/bluepill.desktop
%{_datadir}/icons/hicolor/scalable/apps/bluepill.svg
%{_datadir}/pixmaps/bluepill.png
%{_datadir}/bluepill/imessage-notify.wav
%{_datadir}/imsg/imessage-notify.wav

%changelog
* Fri Nov 20 2026 James Farr Gomez <james@nertwork.com> - 1.2.7-1
- One-click BluePill Cloud browser sign-in with local loopback callback and token exchange, plus v1.2.7 release

* Thu Nov 19 2026 James Farr Gomez <james@nertwork.com> - 1.2.6-1
- BluePill Cloud default login onboarding (desktop client + bluepill-server login), clean BluePill Server wording, and v1.2.6 release

* Thu Nov 19 2026 James Farr Gomez <james@nertwork.com> - 1.2.5-1
- First-class Light / Dark / System mode across Qt6 desktop client and /web UI, plus v1.2.5 release

* Thu Nov 19 2026 James Farr Gomez <james@nertwork.com> - 1.2.4-1
- Built-in Cloud Relay tunnel, unified Liquid Glass/Matrix/Light Web UI, and v1.2.4 release

* Thu Nov 19 2026 James Farr Gomez <james@nertwork.com> - 1.2.3-1
- Cross-platform packaging release for BluePill
