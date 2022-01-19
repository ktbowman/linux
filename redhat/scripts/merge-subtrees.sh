#!/bin/sh

# In newer versions of git-subtree the git repo is not explicitly required,
# however, given the wide variance of git versions we need to include it.

entries="
	redhat/rhdocs git@gitlab.com:redhat/centos-stream/src/kernel/documentation.git
	"

echo $entries | while read entry; do
	git subtree pull --prefix=$entry main
done
