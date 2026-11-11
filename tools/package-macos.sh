#!/usr/bin/env bash
# Package the macOS build (build/dewy.plugin) for release: sign, build the installer, notarize, staple.
#
#   VERSION=0.4.0 tools/package-macos.sh
#
# Environment:
#   VERSION            release version (required)
#   CODESIGN_IDENT     "Developer ID Application: ..." identity; "-" signs ad hoc (local testing only)
#   INSTALLER_IDENT    "Developer ID Installer: ..." identity; with it the output is a signed .pkg,
#                      without it a .zip of the signed plugin
#   NOTARY_APPLE_ID, NOTARY_TEAM_ID, NOTARY_PASSWORD
#                      Apple ID, team ID and app-specific password for notarization; all three set
#                      means notarize + staple
#   UNSIGNED_PKG=1     build the .pkg without an installer identity (local testing only)
#
# Output: dist/dewy-<VERSION>-macos-arm64.pkg (or .zip)
set -euo pipefail

: "${VERSION:?set VERSION}"
CODESIGN_IDENT="${CODESIGN_IDENT:--}"
BUNDLE_ID="io.github.moongoblindev.dewy"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/dewy.plugin"
DIST="$ROOT/dist"
STAGE="$DIST/stage"
NAME="dewy-$VERSION-macos-arm64"

[[ -d "$SRC" ]] || { echo "missing $SRC: build first" >&2; exit 1; }
rm -rf "$STAGE" "$DIST/$NAME.pkg" "$DIST/$NAME.zip" "$DIST/pkg"
mkdir -p "$STAGE" "$DIST/pkg"
ditto "$SRC" "$STAGE/dewy.plugin"
PLUGIN="$STAGE/dewy.plugin"

# ---- sign, inside out: bundled libraries first, then the plugin bundle (hardened runtime, timestamp)
sign() {
	local extra=()
	[[ "$CODESIGN_IDENT" != "-" ]] && extra+=(--timestamp)
	codesign --force --options runtime ${extra[@]+"${extra[@]}"} --sign "$CODESIGN_IDENT" "$@"
}
if [[ -d "$PLUGIN/Contents/Frameworks" ]]; then
	while IFS= read -r -d '' lib; do sign "$lib"; done < <(find "$PLUGIN/Contents/Frameworks" -type f -name "*.dylib" -print0)
fi
sign --identifier "$BUNDLE_ID" "$PLUGIN"
codesign --verify --strict --deep --verbose=2 "$PLUGIN"

notarize() { # notarize <file>
	if [[ -z "${NOTARY_APPLE_ID:-}" || -z "${NOTARY_TEAM_ID:-}" || -z "${NOTARY_PASSWORD:-}" ]]; then
		echo "notarization skipped (no credentials)"
		return 0
	fi
	local out id status
	out=$(xcrun notarytool submit "$1" --apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" \
		--password "$NOTARY_PASSWORD" --wait --timeout 45m --output-format json)
	id=$(/usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])' <<<"$out")
	status=$(/usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin)["status"])' <<<"$out")
	echo "notarization $id: $status"
	if [[ "$status" != "Accepted" ]]; then
		xcrun notarytool log "$id" --apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" \
			--password "$NOTARY_PASSWORD" || true
		return 1
	fi
	NOTARIZED=1
}
NOTARIZED=0

if [[ -n "${INSTALLER_IDENT:-}" || "${UNSIGNED_PKG:-}" == "1" ]]; then
	# ---- installer: into ~/Library/Application Support/obs-studio/plugins (no admin prompt)
	pkgbuild --root "$STAGE" --identifier "$BUNDLE_ID" --version "$VERSION" \
		--install-location "/Library/Application Support/obs-studio/plugins" \
		"$DIST/pkg/dewy.pkg"
	cat >"$DIST/pkg/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8" standalone="no"?>
<installer-gui-script minSpecVersion="1.0">
    <options rootVolumeOnly="true" hostArchitectures="arm64" customize="never" allow-external-scripts="no" />
    <domains enable_currentUserHome="true" enable_anywhere="false" enable_localSystem="false" />
    <title>Dewy $VERSION</title>
    <choices-outline><line choice="dewy" /></choices-outline>
    <choice id="dewy" title="Dewy" description=""><pkg-ref id="$BUNDLE_ID" /></choice>
    <pkg-ref id="$BUNDLE_ID" version="$VERSION">#dewy.pkg</pkg-ref>
</installer-gui-script>
EOF
	sign_args=()
	[[ -n "${INSTALLER_IDENT:-}" ]] && sign_args+=(--sign "$INSTALLER_IDENT" --timestamp)
	productbuild --distribution "$DIST/pkg/distribution.xml" --package-path "$DIST/pkg" ${sign_args[@]+"${sign_args[@]}"} \
		"$DIST/$NAME.pkg"
	[[ -n "${INSTALLER_IDENT:-}" ]] && pkgutil --check-signature "$DIST/$NAME.pkg"
	notarize "$DIST/$NAME.pkg"
	[[ "$NOTARIZED" == 1 ]] && xcrun stapler staple "$DIST/$NAME.pkg" && xcrun stapler validate "$DIST/$NAME.pkg"
	OUT="$DIST/$NAME.pkg"
else
	# ---- no installer identity: a zip of the signed plugin (notarized; the ticket is checked online)
	ditto -c -k --keepParent "$PLUGIN" "$DIST/$NAME.zip"
	notarize "$DIST/$NAME.zip"
	if [[ "$NOTARIZED" == 1 ]] && xcrun stapler staple "$PLUGIN" 2>/dev/null; then
		rm -f "$DIST/$NAME.zip" # re-zip with the ticket stapled to the bundle
		ditto -c -k --keepParent "$PLUGIN" "$DIST/$NAME.zip"
	fi
	OUT="$DIST/$NAME.zip"
fi
rm -rf "$STAGE" "$DIST/pkg"
echo "packaged $OUT"
