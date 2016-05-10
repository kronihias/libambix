#!/bin/sh

WD=$(pwd)
INPUTDIR="${WD}/doc/apiref"
OUTPUTDIR="${WD}/gh-pages"
REMOTE=https://github.com/iem-projects/ambix

if [ -e "${INPUTDIR}/index.html" ]; then
 :
else
 exit 0
fi
if [ "x${REMOTE}" = "x" ]; then
 exit 0
fi
COMMIT=$(git describe --always)

commit_msg() {
echo "Deploy code docs to GitHub Pages"
echo ""
if [ "x${COMMIT}" != x ]; then
 echo "Commit: ${COMMIT}"
fi
if [ "x${TRAVIS_BUILD_NUMBER}" != "x" ]; then
  echo "Travis build: ${TRAVIS_BUILD_NUMBER}"
fi
if [ "x${TRAVIS_COMMIT}" != "x" ]; then
  echo "Travis commit: ${TRAVIS_COMMIT}"
fi
}

git clone -b gh-pages "${REMOTE}" "${OUTPUTDIR}"
cd "${OUTPUTDIR}" || exit 1

##### Configure git.
# Set the push default to simple i.e. push only the current branch.
git config push.default simple
# Pretend to be an user called Travis CI.
git config user.name "Travis CI"
git config user.email "travis@travis-ci.org"

## clean gh-pages
rm -rf "${INPUTDIR##*/}"

## copy the doxygen documentation
cp -rav "${INPUTDIR}" "${INPUTDIR##*/}"

## add and commit to git
git add --all "${INPUTDIR##*/}"
commit_msg | git commit "${INPUTDIR##*/}" -F -


# and push
if [ "x${GH_REPO_TOKEN}" != "x" ]; then
  GH_REPO_REF=${REMOTE#*@}
  GH_REPO_REF=${GH_REPO_REF#*//}
  git push --force "https://${GH_REPO_TOKEN}@${GH_REPO_REF}" > /dev/null
else
  git push --force > /dev/null
fi

rm -rf "${OUTPUTDIR}"
