#!/bin/sh

SOURCES=$1
SPECFILE=$2
CHANGELOG=$3
PKGRELEASE=$4
KVERSION=$5
KPATCHLEVEL=$6
KSUBLEVEL=$7
DISTRO_BUILD=$8
RELEASED_KERNEL=$9
SPECRELEASE=${10}
BUILDOPTS=${11}
PACKAGE_NAME=${12}
MARKER=${13}
RHEL_MAJOR=${14}
RHEL_MINOR=${15}
RPMVERSION=${KVERSION}.${KPATCHLEVEL}.${KSUBLEVEL}
clogf="$SOURCES/changelog"
# hide [redhat] entries from changelog
HIDE_REDHAT=1;
# hide entries for unsupported arches
HIDE_UNSUPPORTED_ARCH=1;
# override LC_TIME to avoid date conflicts when building the srpm
LC_TIME=
STAMP=$(echo $MARKER | cut -f 1 -d '-' | sed -e "s/v//");
RPM_VERSION="$RPMVERSION-$PKGRELEASE";

GIT_FORMAT="--format=- %s (%an)%n%N%n^^^NOTES-END^^^%n%b"
GIT_NOTES="--notes=refs/notes/${RHEL_MAJOR}.${RHEL_MINOR}*"

# We want to exclude changes in redhat/rhdocs tree from the changelog output.
# Since the redhat/rhdocs is a separate git subtree, we can exclude the full
# list of commits with "^" specifier against latest child from the subtree.
# This is done this way to be compatible with old git versions which do not
# have the pathspec '(exclude)' support
EXCLUDE=$(git log -1 --format=%P ${0%/*}/rhdocs | cut -d ' ' -f 2)

echo >$clogf

lasttag=$(git rev-list --first-parent --grep="^\[redhat\] ${PACKAGE_NAME}-${RPMVERSION}" --max-count=1 HEAD)
# if we didn't find the proper tag, assume this is the first release
if [ -z "$lasttag" ]; then
	lasttag=$(git describe --match="$MARKER" --abbrev=0)
fi
echo "Gathering new log entries since $lasttag"
git log --topo-order --reverse --no-merges -z $GIT_NOTES "$GIT_FORMAT" \
	${lasttag}.. ${EXCLUDE:+^$EXCLUDE} | ${0%/*}/genlog.py >> "$clogf"

cat $clogf | grep -v "tagging $RPM_VERSION" > $clogf.stripped
cp $clogf.stripped $clogf

if [ "x$HIDE_REDHAT" == "x1" ]; then
	cat $clogf | grep -v -e "^- \[redhat\]" |
		sed -e 's!\[Fedora\]!!g' > $clogf.stripped
	cp $clogf.stripped $clogf
fi

if [ "x$HIDE_UNSUPPORTED_ARCH" == "x1" ]; then
	cat $clogf | egrep -v "^- \[(alpha|arc|arm|avr32|blackfin|c6x|cris|frv|h8300|hexagon|ia64|m32r|m68k|metag|microblaze|mips|mn10300|openrisc|parisc|score|sh|sparc|tile|um|unicore32|xtensa)\]" > $clogf.stripped
	cp $clogf.stripped $clogf
fi

LENGTH=$(wc -l $clogf | awk '{print $1}')

#the changelog was created in reverse order
#also remove the blank on top, if it exists
#left by the 'print version\n' logic above
cname="$(git var GIT_COMMITTER_IDENT |sed 's/>.*/>/')"
cdate="$(LC_ALL=C date +"%a %b %d %Y")"
cversion="[$RPM_VERSION]";
tac $clogf | sed "1{/^$/d; /^- /i\
* $cdate $cname $cversion
	}" > $clogf.rev

if [ "$LENGTH" = 0 ]; then
	rm -f $clogf.rev; touch $clogf.rev
fi

cat $clogf.rev $CHANGELOG > $clogf.full
mv -f $clogf.full $CHANGELOG

test -n "$SPECFILE" &&
        sed -i -e "
	/%%CHANGELOG%%/r $CHANGELOG
	/%%CHANGELOG%%/d
	s/%%PACKAGE_NAME%%/$PACKAGE_NAME/
	s/%%KVERSION%%/$KVERSION/
	s/%%KPATCHLEVEL%%/$KPATCHLEVEL/
	s/%%KSUBLEVEL%%/$KSUBLEVEL/
	s/%%PKGRELEASE%%/$PKGRELEASE/
	s/%%SPECRELEASE%%/$SPECRELEASE/
	s/%%DISTRO_BUILD%%/$DISTRO_BUILD/
	s/%%RELEASED_KERNEL%%/$RELEASED_KERNEL/" $SPECFILE

for opt in $BUILDOPTS; do
	add_opt=
	[ -z "${opt##+*}" ] && add_opt="_with_${opt#?}"
	[ -z "${opt##-*}" ] && add_opt="_without_${opt#?}"
	[ -n "$add_opt" ] && sed -i "s/^\\(# The following build options\\)/%define $add_opt 1\\n\\1/" $SPECFILE
done

rm -f $clogf{,.rev,.stripped};

