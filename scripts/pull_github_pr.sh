#!/bin/bash

# Script for pulling a github pull request
# along with generating a merge commit message.
# Example usage for pull request #6007:
#
# git checkout master
# git pull
# ./scripts/pull_github_pr.sh 6007
#
# By default a single-commit pull request is cherry-picked, while a
# multi-commit one is merged with a merge commit. With --squash all the
# commits of the pull request are squashed into a single commit that uses
# the pull request cover letter (its title and description) as the message:
#
# ./scripts/pull_github_pr.sh --squash 6007

set -e

usage() {
    echo "Usage: $0 [--squash] <pull-request-number>"
}

SQUASH=
while [[ "$1" == -* ]]; do
    case "$1" in
        --squash)
            SQUASH=yes
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option $1"
            usage
            exit 1
            ;;
    esac
    shift
done

gh_hosts=~/.config/gh/hosts.yml
FORCE="$2"

if [[ ( -z "$GITHUB_LOGIN" || -z "$GITHUB_TOKEN" ) && -f "$gh_hosts" ]]; then
    GITHUB_LOGIN=$(awk '/user:/ { print $2 }' "$gh_hosts")
    GITHUB_TOKEN=$(awk '/oauth_token:/ { print $2 }' "$gh_hosts")
fi

if [[ -z "$GITHUB_LOGIN" || -z "$GITHUB_TOKEN" ]]; then
    echo 'Please set $GITHUB_LOGIN and $GITHUB_TOKEN'
    exit 1
fi

if [[ -z "$1" ]]; then
    echo Please provide a github pull request number
    usage
    exit 1
fi

for required in jq curl; do
    if ! type $required >& /dev/null; then
        echo Please install $required first
        exit 1
    fi
done

do_curl() {
    curl --user "${GITHUB_LOGIN}:${GITHUB_TOKEN}" "$@"
}


NL=$'\n'

PR_NUM="$1"
# convert full repo URL to its project/repo part, in case of failure default to origin/master:
REMOTE_SLASH_BRANCH="$(git rev-parse --abbrev-ref --symbolic-full-name @{upstream} \
     || git rev-parse --abbrev-ref --symbolic-full-name master@{upstream} \
     || echo 'origin/master')"
REMOTE="${REMOTE_SLASH_BRANCH%/*}"
REMOTE_URL="$(git config --get "remote.$REMOTE.url")"
PROJECT=`sed 's/git@github.com://;s#https://github.com/##;s/\.git$//;' <<<"${REMOTE_URL}"`
PR_PREFIX=https://api.github.com/repos/$PROJECT/pulls

echo "Fetching info on PR #$PR_NUM..."
PR_DATA="$(do_curl -s "$PR_PREFIX/$PR_NUM")"
MESSAGE="$(jq -r .message <<< "$PR_DATA")"
if [[ "$MESSAGE" != null ]]; then
    # Error message, probably "Not Found".
    echo "$MESSAGE"
    exit 1
fi
PR_TITLE="$(jq -r .title <<< "$PR_DATA")"
echo "    $PR_TITLE"
PR_DESCR="$(jq -r .body <<< "$PR_DATA")"
PR_LOGIN="$(jq -r .head.user.login <<< "$PR_DATA")"
echo -n "Fetching full name of author $PR_LOGIN... "
USER_NAME="$(do_curl -s "https://api.github.com/users/$PR_LOGIN" | jq -r .name)"
echo "$USER_NAME"

git fetch "$REMOTE" "pull/$PR_NUM/head"

nr_commits=$(git log --pretty=oneline HEAD..FETCH_HEAD | wc -l)

closes="${NL}${NL}Closes ${PROJECT}#${PR_NUM}${NL}"

if [[ -n "$SQUASH" ]]; then
    # Squash the whole pull request into a single commit, using the pull
    # request cover letter as the commit message. The commit is attributed
    # to the author of the pull request's first commit.
    author="$(git log --format="format:%an <%ae>" HEAD..FETCH_HEAD | tail -1)"
    # github serves the description with CRLF line endings, and reports an
    # empty cover letter as a null
    descr="${PR_DESCR//$'\r'/}"
    [[ "$descr" == null ]] && descr=""
    if ! git merge --squash FETCH_HEAD; then
        echo "Squash failed. You are now in a subshell. Resolve the conflicts and git add them, but do not commit, then exit the subshell. To give up, run git merge --abort first"
        bash
        if [[ -n "$(git ls-files --unmerged)" ]]; then
            echo "Conflicts are still unresolved"
            exit 1
        fi
        if git diff --cached --quiet; then
            echo "Nothing to commit"
            exit 1
        fi
    fi
    git commit --author="$author" -m "$PR_TITLE" -m "${descr}${closes}"
elif [[ $nr_commits == 1 ]]; then
    commit=$(git log --pretty=oneline HEAD..FETCH_HEAD | awk '{print $1}')
    message="$(git log -1 "$commit" --format="format:%s%n%n%b")"
    if ! git cherry-pick "$commit"; then
        echo "Cherry-pick failed. You are now in a subshell. Either resolve with git cherry-pick --continue or git cherry-pick --abort, then exit the subshell"
        head_before="$(git rev-parse HEAD)"
        bash
        head_after="$(git rev-parse HEAD)"
        if [[ "$head_before" = "$head_after" ]]; then
            exit 1
        fi
    fi
    git commit --amend -m "${message}${closes}"
else
    git merge --no-ff --log=1000 FETCH_HEAD -m "Merge '$PR_TITLE' from $USER_NAME" -m "${PR_DESCR}${closes}"
fi
git commit --amend # for a manual double-check
