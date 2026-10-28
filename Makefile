# SPDX-License-Identifier: GPL-3.0-or-later
#
# make linux           -> build/usbeam            Linux engine, static (for testing by hand)
# make linux-pkg       -> build/usbeam-dyn        Linux engine, for packages
# make linux-app       -> build/usbeam-app        Linux app window + tray (needs Qt 6 WebEngine)
# make windows-arm64   -> build/usbeam-service-arm64.exe + build/usbeam-app-arm64.exe
# make windows-x64     -> build/usbeam-service-x64.exe   + build/usbeam-app-x64.exe
#     (Windows: needs llvm-mingw in PATH and the WebView2 SDK unpacked in build/webview2-sdk)
# make webview2        -> downloads the WebView2 SDK from NuGet into build/webview2-sdk

CFLAGS ?= -O2 -Wall
WINLIBS = -lsetupapi -lwinusb -lws2_32 -lcfgmgr32 -liphlpapi -lshell32 -ladvapi32 -luser32
WV2 = build/webview2-sdk/build/native

all: linux windows-arm64 windows-x64

build:
	mkdir -p build

build/ui_html.h: src/common/ui.html | build
	python3 -c "d=open('src/common/ui.html','rb').read(); print('const unsigned char ui_html[] = {' + ','.join(str(b) for b in d) + '};'); print('const unsigned int ui_html_len = %d;' % len(d))" > build/ui_html.h

linux: build/ui_html.h
	gcc $(CFLAGS) -Ibuild -static -pthread -o build/usbeam src/linux/usbeam.c

linux-pkg: build/ui_html.h
	gcc $(CFLAGS) -Ibuild -pthread -o build/usbeam-dyn src/linux/usbeam.c

linux-app: | build
	g++ -std=c++17 -O2 -Wall -fPIC src/linux/app/usbeam_app.cpp -o build/usbeam-app \
		$$(pkg-config --cflags --libs Qt6Widgets Qt6WebEngineWidgets Qt6Network)

webview2: | build
	mkdir -p build/webview2-sdk
	curl -sSL -o build/webview2.zip https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2
	cd build/webview2-sdk && unzip -oq ../webview2.zip
	mkdir -p build/webview2/arm64 build/webview2/x64 build/shim
	cp $(WV2)/arm64/WebView2Loader.dll build/webview2/arm64/
	cp $(WV2)/x64/WebView2Loader.dll build/webview2/x64/
	printf '#include <eventtoken.h>\n' > build/shim/EventToken.h

windows-%: build/ui_html.h
	$(eval ARCH := $(if $(filter arm64,$*),aarch64,x86_64))
	cd src/windows && $(ARCH)-w64-mingw32-windres app.rc -O coff -o ../../build/svc-$*.res
	$(ARCH)-w64-mingw32-gcc $(CFLAGS) -Ibuild -Wno-missing-field-initializers src/windows/usbeam_host.c build/svc-$*.res \
		-mwindows -o build/usbeam-service-$*.exe $(WINLIBS)
	cd src/windows/app && $(ARCH)-w64-mingw32-windres app.rc -O coff -o ../../../build/app-$*.res
	$(ARCH)-w64-mingw32-clang++ -std=c++17 -O2 -Wall -municode -mwindows -static -Ibuild/shim -I$(WV2)/include \
		src/windows/app/usbeam_app.cpp build/app-$*.res -o build/usbeam-app-$*.exe \
		-lws2_32 -ldwmapi -lshell32 -lole32 -luser32 -ladvapi32

clean:
	rm -rf build

.PHONY: all linux linux-pkg linux-app webview2 clean
