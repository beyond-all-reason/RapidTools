#!/bin/sh

ionice -p $$ -c 3
renice 20 -p $$

MAXLOAD=3
LOAD=$(uptime | egrep -o -e "load average: [0-9]*"|cut -b 15-)
if [ $LOAD -ge $MAXLOAD ]; then
	echo current load $LOAD is higher than maxload $MAXLOAD, aborting svn sync
	exit
fi


# checks all 
MODROOT=/
MODINFO=modinfo.lua
PACKAGES=/home/packages/www

REPOS=$(find /home/packages/git -maxdepth 1 -mindepth 1 -type d)


for REPO in $REPOS; do
	cd $REPO
	git fetch
	LOCAL=$(git rev-parse HEAD)
	REMOTE=$(git rev-parse @{u})
	if [ "$LOCAL" != "$REMOTE" ]; then
		TAG=$(basename $REPO)
		echo Updateing $REPO
		git pull
		git checkout master
		git reset --hard origin/master
		~/bin/BuildGit "$REPO" "$MODROOT" "$MODINFO" "$PACKAGES/$TAG" "$REMOTE" "$TAG"
		git log -1 --pretty=format:"%an commited %h: %s" | ~/bin/loggit.py "$TAG"
	fi
done

