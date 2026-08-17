#!/bin/sh

BASELOG=/var/log/messages
MIIO_CLIENT_LOG=/var/log/miio_client.log
corp=`nvram factory get model`
did=`nvram factory get did`
id="messages"
timestamp=`date +%Y-%m-%d-%T`
server="https://dlg.io.mi.com/v1/upload"

IMI_BASEFILE=/mnt/data/imi
if [ -e ${IMI_BASEFILE} ]; then
	echo "imi do not upload"
	exit 1
fi

sanity_check() {
    if type curl > /dev/null ; then
	echo "Found curl"
    else
	echo "Can not find curl"
	exit 1
    fi
}

reportone () {
    echo "reporting $1"
	response=`curl -k -X POST -F "corp=$corp" -F "did=$did" -F "id=$id" -F "ts=$timestamp" -F "data=<$1" $server 2> /dev/null`
	if echo $response | grep -q success; then
	if [ x$1 = x${BASELOG} ]; then
		:> ${BASELOG}
	elif [ x$1 = x${MIIO_CLIENT_LOG} ]; then
		:> ${MIIO_CLIENT_LOG}
	else
		rm -f $1
	fi
	fi
}

main() {
    logn=`ls ${BASELOG}* | sort | wc -l`
    logn=$(expr $logn - 2)

    until [ $logn -lt 0 ]
    do
	file=${BASELOG}.$logn
	if [ -e $file ]; then
	    reportone $file
	fi

	logn=$(expr $logn - 1)
    done

    reportone ${BASELOG}
	if [ ! -e ${IMI_LOGFILE} ]; then
    reportone ${MIIO_CLIENT_LOG}
	fi
}

sanity_check
main
