#!/usr/bin/env bash
# shellcheck source=packaging/release/common.sh
source "$(dirname "$0")/common.sh"
assets=$1
cd "$assets"
sha256sum -c SHA256SUMS
[[ $(git -C "$src" rev-parse HEAD) == "$GITHUB_SHA" ]]

ref="refs/tags/$TAG"
remote=$(git -C "$src" ls-remote origin "$ref" "$ref^{}")
if [[ $PUBLISH == true ]]; then
    [[ $GITHUB_EVENT_NAME == push && $GITHUB_REF == "$ref" ]]
    # An annotated tag peels to the commit on its second ls-remote line.
    [[ $(awk 'END {print $1}' <<< "$remote") == "$GITHUB_SHA" ]]
    flags=(--verify-tag --prerelease)
else
    [[ -z $remote ]] || { echo 'Draft name already has a Git tag; choose a new name.' >&2; exit 1; }
    flags=(--draft --target "$GITHUB_SHA")
fi

# Never overwrite a published release or owner-created draft. Only replace
# drafts carrying this workflow's marker. Deleting via the API cannot delete refs.
repo=${GITHUB_REPOSITORY:?}
existing=$(gh api --paginate "repos/$repo/releases?per_page=100" | \
    jq -cs --arg tag "$TAG" '[.[][] | select(.tag_name==$tag)]')
jq -e 'all(.[]; .draft and ((.body // "") | contains("<!-- metadesk-release-workflow -->")))' \
    <<< "$existing" >/dev/null
for id in $(jq -r '.[].id' <<< "$existing"); do
    gh api --method DELETE "repos/$repo/releases/$id"
done

files=()
for file in *; do
    [[ $file == release-notes.md ]] || files+=("$file")
done
url=$(gh release create "$TAG" "${files[@]}" -R "$repo" "${flags[@]}" \
    --latest=false --title "$TAG" --notes-file release-notes.md)
if [[ $PUBLISH != true ]]; then
    [[ -z $(git -C "$src" ls-remote origin "$ref") ]]
fi
printf 'Release: %s\n' "$url" | tee -a "$GITHUB_STEP_SUMMARY"
