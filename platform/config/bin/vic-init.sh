#!/bin/sh -f
#
# platform/config/bin/vic-init.sh
#
# Perform any cleanup & init tasks not handled by the OS.
#
# This script runs each time the service group (anki-robot.target)
# is started, BEFORE any individual vic-blah services.
#

#
# Forward log events from rampost, if any
#
RAMPOST_LOG=/dev/rampost.log

if [ -f ${RAMPOST_LOG} ]; then
  /anki/bin/vic-log-forward rampost ${RAMPOST_LOG}
  /bin/rm -f ${RAMPOST_LOG}
fi

#
# Forward kernel panics, if any
#
PANICS_DIR="/data/panics"
if [ -d "${PANICS_DIR}" ]; then
  shopt -s nullglob
  for i in "${PANICS_DIR}"/*
  do
    echo "/anki/bin/vic-log-kernel-panic ${i}"
    if /anki/bin/vic-log-kernel-panic "${i}" ; then
      echo "/bin/rm -f ${i}"
      /bin/rm -f "${i}"
    fi
  done
  shopt -u nullglob
fi

#
# After CLEAR OUT SOUL, mount-data only creates /data/data/com.anki.victor.
# If wired (root) mkdir's persistent/ first, it becomes root:root 755 and
# vic-switchboard (User=net) cannot create sessions → fault 913.
#
VICTOR_DATA="/data/data/com.anki.victor"
mkdir -p "${VICTOR_DATA}/persistent/switchboard" "${VICTOR_DATA}/cache"
chown anki:anki "${VICTOR_DATA}" "${VICTOR_DATA}/persistent" "${VICTOR_DATA}/cache" 2>/dev/null || true
chmod 770 "${VICTOR_DATA}" "${VICTOR_DATA}/persistent" "${VICTOR_DATA}/cache" "${VICTOR_DATA}/persistent/switchboard" 2>/dev/null || true
chown -R net:anki "${VICTOR_DATA}/persistent/switchboard" 2>/dev/null || true

#
# Clear fault code, if any
#
/bin/fault-code-clear
