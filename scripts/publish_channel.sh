#!/usr/bin/env bash
# Owns the channel file format that Check for Updates downloads.
# Usage: publish_channel.sh VERSION CHANNEL
# Requires GH_TOKEN and GITHUB_REPOSITORY.
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: publish_channel.sh VERSION CHANNEL" >&2
  exit 2
fi
version="$1"
channel="$2"
: "${GH_TOKEN:?GH_TOKEN is required}"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
repo=(--repo "$GITHUB_REPOSITORY")

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "VERSION must match MAJOR.MINOR.PATCH" >&2
  exit 1
fi
case "$channel" in
  stable | beta | nightly) ;;
  *)
    echo "CHANNEL must be stable, beta, or nightly" >&2
    exit 1
    ;;
esac

asset="oinkos-${version}-x3-x4.bin"
asset_json="$(gh release view "$version" "${repo[@]}" --json assets |
  jq -e --arg name "$asset" '.assets[] | select(.name == $name)')" || {
  echo "Release ${version} has no asset ${asset}" >&2
  exit 1
}

if ! gh release view channels "${repo[@]}" > /dev/null 2>&1; then
  gh release create channels "${repo[@]}" --prerelease --latest=false \
    --title "channels" --notes "Check for Updates reads these files."
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
out_file="${work_dir}/${channel}.json"

current=""
if gh release download channels "${repo[@]}" --pattern "${channel}.json" --output "${work_dir}/current.json" 2> /dev/null; then
  current="$(jq -r '.tag_name // empty' "${work_dir}/current.json")"
fi

# Channels only move forward, so runs that finish out of order cannot roll one back.
if [[ -n "$current" && "$current" != "$version" &&
  "$(printf '%s\n%s\n' "$current" "$version" | sort -V | tail -n1)" == "$current" ]]; then
  echo "${channel}.json is already ${current}; not moving it back to ${version}"
  # A promotion is a human request, so refusing it must fail the run.
  [[ "$channel" == "stable" ]] && exit 1
  exit 0
fi

if [[ "$current" != "$version" ]]; then
  # tag_name must come first: the firmware picks the asset name when it reads the tag.
  jq -c '{tag_name: $version, assets: [{name, browser_download_url: .url, size}]}' \
    --arg version "$version" <<< "$asset_json" > "$out_file"
  gh release upload channels "$out_file" "${repo[@]}" --clobber
fi

if [[ "$channel" == "stable" ]]; then
  gh release edit "$version" "${repo[@]}" --prerelease=false --latest
fi
