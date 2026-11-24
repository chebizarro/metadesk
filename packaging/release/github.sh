#!/usr/bin/env bash
# shellcheck source=packaging/release/common.sh
source "$(dirname "$0")/common.sh"
die() { echo "$*" >&2; exit 1; }
assets=$1
cd "$assets"
sha256sum -c SHA256SUMS
commit=$(git -C "$src" rev-parse "$GITHUB_SHA^{commit}")
[[ $(git -C "$src" rev-parse HEAD) == "$commit" ]] || die 'Checkout does not match trigger SHA.'

ref="refs/tags/$TAG"
remote=$(git -C "$src" ls-remote origin "$ref" "$ref^{}")
case $PUBLISH in
    true)
        [[ $GITHUB_EVENT_NAME == push && $GITHUB_REF == "$ref" ]] || die 'Publishing requires a tag push.'
        # An annotated tag peels to its commit on the second ls-remote line.
        [[ $(awk 'END {print $1}' <<< "$remote") == "$commit" ]] || die 'Remote tag is missing or has moved.'
        flags=(--verify-tag --prerelease) ;;
    false)
        [[ -z $remote ]] || die 'Draft name already has a Git tag; choose a new name.'
        flags=(--draft --target "$commit") ;;
    *) die 'Unknown release mode.' ;;
esac

# Never overwrite a published release or owner-created draft. Only replace
# drafts carrying this workflow's marker. Deleting via the API cannot delete refs.
repo=${GITHUB_REPOSITORY:?}
existing=$(gh api --paginate "repos/$repo/releases?per_page=100" | \
    jq -cs --arg tag "$TAG" '[.[][] | select(.tag_name==$tag)]')
jq -e 'all(.[]; .draft and ((.body // "") | contains("<!-- metadesk-release-workflow -->")))' \
    <<< "$existing" >/dev/null || die 'Refusing to replace a non-workflow draft or published release.'
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
    created_ref=$(git -C "$src" ls-remote origin "$ref")
    [[ -z $created_ref ]] || die 'Unexpected tag after draft creation.'
fi
printf 'Release: %s\n' "$url" | tee -a "$GITHUB_STEP_SUMMARY"
