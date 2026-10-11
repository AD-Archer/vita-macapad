# Macapad for PS Vita tasks. `just` lists them. Builds run in VitaSDK's Docker image (Docker Desktop must be running).

emulator := justfile_directory() / "../.tools/vita3k-dl/Vita3K.app"

default:
	@just --list

# Build macapad.vpk (always clean)
build:
	./build.sh

# Build and install it in the Vita3K emulator, which then opens it
run: build
	-pkill -f "Vita3K.app/Contents/MacOS/Vita3K"
	open -n "{{emulator}}" --args "{{justfile_directory()}}/macapad.vpk"

# A pretend Macapad on 127.0.0.1:7399, to try the app in Vita3K without pairing your Mac
mock:
	python3 scripts/mock-macapad.py

# Build and copy the VPK to ux0:data/ over VitaShell's FTP (press SELECT in VitaShell first), e.g. `just send 10.0.0.42`
send ip: build
	curl --ftp-method nocwd -T macapad.vpk "ftp://{{ip}}:1337/ux0:/data/"
	@echo "Now install ux0:data/macapad.vpk from VitaShell."

# Once it's installed: build and replace just the app's eboot.bin over FTP (quicker than a VPK)
eboot ip: build
	curl --ftp-method nocwd -T build/eboot.bin "ftp://{{ip}}:1337/ux0:/app/MCPD07317/"

# Make the LiveArea images in sce_sys/ again from assets/
assets:
	python3 scripts/make-assets.py

# Remove build output
clean:
	./build.sh clean
