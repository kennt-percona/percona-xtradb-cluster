#!/bin/bash -ue
# Copyright (C) 2013 Percona Inc
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not, write to the
# Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston
# MA  02110-1301  USA.

# Documentation: http://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html
# Make sure to read that before proceeding!


#-------------------------------------------------------------------------------
#
# Step-1: Parse and read input params arguments. These are mainly
# related to role/username/password/etc...
#
. $(dirname $0)/wsrep_sst_common

#-------------------------------------------------------------------------------
#
# Step-2: Setup default global variable that we plan to use during processing.
#

# encryption specific variables.
ealgo=""
ekey=""
ekeyfile=""
encrypt=0

nproc=1
ecode=0
ssyslog=""
ssystag=""
XTRABACKUP_PID=""
SST_PORT=""
REMOTEIP=""
tca=""
tcert=""
tkey=""
sockopt=""
progress=""
ttime=0
totime=0
lsn=""
ecmd=""
rlimit=""
stagemsg="${WSREP_SST_OPT_ROLE}"
cpat=""
ib_home_dir=""
ib_log_dir=""
ib_undo_dir=""
sfmt="tar"
strmcmd=""
tfmt=""
tcmd=""
rebuild=0
rebuildcmd=""
payload=0
pvformat="-F '%N => Rate:%r Avg:%a Elapsed:%t %e Bytes: %b %p' "
pvopts="-f  -i 10 -N $WSREP_SST_OPT_ROLE "
STATDIR=""
uextra=0
disver=""

# keyring specific variables.
keyring=""
keyringsid=""
keyringbackupopt=""
keyringapplyopt=""
XB_DONOR_KEYRING_FILE="donor-keyring"
XB_DONOR_KEYRING_FILE_PATH=""
KEYRING_DIR=""

tmpopts=""
itmpdir=""
xtmpdir=""

scomp=""
sdecomp=""
ssl_dhparams=""
pxc_encrypt_cluster_traffic=""

ssl_cert=""
ssl_ca=""
ssl_key=""

# Required for backup locks
# For backup locks it is 1 sent by joiner
# 5.6.21 PXC and later can't donate to an older joiner
sst_ver=1

# pv is used to to meter the data passing through.
# more for statiscally reporting.
if which pv &>/dev/null && pv --help | grep -q FORMAT; then
    pvopts+=$pvformat
fi
pcmd="pv $pvopts"
declare -a RC

# default XB (xtrabackup-binary) to use.
INNOBACKUPEX_BIN=xtrabackup
DATA="${WSREP_SST_OPT_DATA}"

# These files carry some important information in form of GTID of the data
# that is being backed up.
XB_GTID_INFO_FILE="xtrabackup_galera_info"
XB_GTID_INFO_FILE_PATH="${DATA}/${XB_GTID_INFO_FILE}"
IST_FILE="xtrabackup_ist"

# ss: is utility to investigate socket, ip for network routes.
export PATH="/usr/sbin:/sbin:$PATH"

#-------------------------------------------------------------------------------
#
# Step-3: Helper function to carry-out the main workload.
#

#
# time each of the command that we run.
timeit()
{
    local stage=$1
    shift
    local cmd="$@"
    local x1 x2 took extcode

    if [[ $ttime -eq 1 ]]; then
        x1=$(date +%s)
        wsrep_log_info "Evaluating $cmd"
        eval "$cmd"
        extcode=$?
        x2=$(date +%s)
        took=$(( x2-x1 ))
        wsrep_log_info "NOTE: $stage took $took seconds"
        totime=$(( totime+took ))
    else
        wsrep_log_info "Evaluating $cmd"
        eval "$cmd"
        extcode=$?
    fi
    return $extcode
}

#
# get encryption keys.
get_keys()
{
    # $encrypt -eq 1 is for internal purposes only
    if [[ $encrypt -ge 2 || $encrypt -eq -1 ]]; then
        return
    fi

    if [[ $encrypt -eq 0 ]]; then
        if $MY_PRINT_DEFAULTS -c $WSREP_SST_OPT_CONF xtrabackup | grep -q encrypt; then
            wsrep_log_error "Unexpected option combination. SST may fail. Refer to http://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html "
        fi
        return
    fi

    if [[ $sfmt == 'tar' ]]; then
        wsrep_log_info "NOTE: Xtrabackup-based encryption - encrypt=1 - cannot be enabled with tar format"
        encrypt=-1
        return
    fi

    wsrep_log_info "Xtrabackup based encryption enabled in my.cnf - Supported only from Xtrabackup 2.1.4"

    if [[ -z $ealgo ]]; then
        wsrep_log_error "FATAL: Encryption algorithm empty from my.cnf, bailing out"
        exit 3
    fi

    if [[ -z $ekey && ! -r $ekeyfile ]]; then
        wsrep_log_error "FATAL: Either key or keyfile must be readable"
        exit 3
    fi

    if [[ -z $ekey ]]; then
        ecmd="xbcrypt --encrypt-algo=$ealgo --encrypt-key-file=$ekeyfile"
    else
        wsrep_log_warning "Using the 'encrypt-key' option causes the encryption key"
        wsrep_log_warning "to be set via the command-line and is considered insecure."
        wsrep_log_warning "It is recommended to use the 'encrypt-key-file' option instead."

        ecmd="xbcrypt --encrypt-algo=$ealgo --encrypt-key=$ekey"
    fi

    if [[ "$WSREP_SST_OPT_ROLE" == "joiner" ]]; then
        ecmd+=" -d"
    fi

    stagemsg+="-XB-Encrypted"
}

#
# If the ssl_dhparams variable is already set, uses that as a source
# of dh parameters for OpenSSL. Otherwise, looks for dhparams.pem in the
# datadir, and creates it there if it can't find the file.
# No input parameters
#
check_for_dhparams()
{
    if [[ -z "$ssl_dhparams" ]]; then
        if ! [[ -r "$DATA/dhparams.pem" ]]; then
            wsrep_check_programs openssl
            wsrep_log_info "Could not find dhparams file, creating $DATA/dhparams.pem"

            if ! openssl dhparam -out "$DATA/dhparams.pem" 2048 >/dev/null 2>&1
            then
                wsrep_log_error "******** FATAL ERROR ********************************* "
                wsrep_log_error "* Could not create the dhparams.pem file with OpenSSL. "
                wsrep_log_error "****************************************************** "
                exit 22
            fi
        fi
        ssl_dhparams="$DATA/dhparams.pem"
    fi
}

#
# verifies that the certificate matches the private key
# doing this will save us having to wait for a timeout that would
# otherwise occur.
#
# 1st param: path to the cert
# 2nd param: path to the private key
#
verify_cert_matches_key()
{
    local cert_path=$1
    local key_path=$2

    wsrep_check_programs openssl diff

    # generate the public key from the cert and the key
    # they should match (otherwise we can't create an SSL connection)
    if ! diff <(openssl x509 -in "$cert_path" -pubkey -noout) <(openssl rsa -in "$key_path" -pubout 2>/dev/null) >/dev/null 2>&1
    then
        wsrep_log_error "******** FATAL ERROR ************************* "
        wsrep_log_error "* The certifcate and private key do not match. "
        wsrep_log_error "* Please check your certificate and key files. "
        wsrep_log_error "********************************************** "
        exit 22
    fi
}

#
# Checks to see if the file exists
# If the file does not exist (or cannot be read), issues an error
# and exits
#
# 1st param: file name to be checked (for read access)
# 2nd param: 1st error message (header)
# 3rd param: 2nd error message (footer, optional)
#
verify_file_exists()
{
    local file_path=$1
    local error_message1=$2
    local error_message2=$3

    if ! [[ -r "$file_path" ]]; then
        wsrep_log_error "******** FATAL ERROR ************************* "
        wsrep_log_error "* $error_message1 "
        wsrep_log_error "* Could not find/access : $file_path "

        if ! [[ -z "$error_message2" ]]; then
            wsrep_log_error "* $error_message2 "
        fi

        wsrep_log_error "********************************************** "
        exit 22
    fi
}

#
# Setup stream to transfer. (Alternative: nc or socat)
get_transfer()
{
    if [[ -z $SST_PORT ]]; then
        TSST_PORT=4444
    else
        TSST_PORT=$SST_PORT
    fi

    if [[ $tfmt == 'nc' ]]; then
        if [[ ! -x `which nc` ]]; then
            wsrep_log_error "nc(netcat) not found in path: $PATH"
            exit 2
        fi
        wsrep_log_info "Using netcat as streamer"
        if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]]; then
            if nc -h 2>&1 | grep -q ncat; then
                tcmd="nc -l ${TSST_PORT}"
            else
                tcmd="nc -dl ${TSST_PORT}"
            fi
        else
            tcmd="nc ${REMOTEIP} ${TSST_PORT}"
        fi
    else
        tfmt='socat'
        wsrep_log_info "Using socat as streamer"
        if [[ ! -x `which socat` ]]; then
            wsrep_log_error "socat not found in path: $PATH"
            exit 2
        fi

        donor_extra=""
        joiner_extra=""
        if [[ $encrypt -eq 2 || $encrypt -eq 3 || $encrypt -eq 4 ]]; then
            if ! socat -V | grep -q WITH_OPENSSL; then
                wsrep_log_error "******** FATAL ERROR ****************** "
                wsrep_log_error "* socat is not openssl enabled.         "
                wsrep_log_error "* Unable to encrypt SST communications. "
                wsrep_log_error "*************************************** "
                exit 2
            fi

            # Determine the socat version
            SOCAT_VERSION=`socat -V 2>&1 | grep -oe '[0-9]\.[0-9][\.0-9]*' | head -n1`
            if [[ -z "$SOCAT_VERSION" ]]; then
                wsrep_log_error "******** FATAL ERROR **************** "
                wsrep_log_error "* Cannot determine the socat version. "
                wsrep_log_error "************************************* "
                exit 2
            fi

            # socat versions < 1.7.3 will have 512-bit dhparams (too small)
            #       so create 2048-bit dhparams and send that as a parameter
            # socat version >= 1.7.3, checks to see if the peername matches the hostname
            #       set commonname="" to disable the peername checks
            #
            if ! check_for_version "$SOCAT_VERSION" "1.7.3"; then
                if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]]; then
                    # dhparams check (will create ssl_dhparams if needed)
                    check_for_dhparams
                    joiner_extra=",dhparam=$ssl_dhparams"
                fi
            fi
            if check_for_version "$SOCAT_VERSION" "1.7.3"; then
                donor_extra=',commonname=""'
            fi
        fi

        if [[ $encrypt -eq 2 ]]; then
            wsrep_log_warning "**** WARNING **** encrypt=2 is deprecated and will be removed in a future release"
            wsrep_log_info "Using openssl based encryption with socat: with crt and ca"

            verify_file_exists "$tcert" "Both certificate and CA files are required." \
                                        "Please check the 'tcert' option.           "
            verify_file_exists "$tca" "Both certificate and CA files are required." \
                                      "Please check the 'tca' option.             "

            stagemsg+="-OpenSSL-Encrypted-2"
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
                wsrep_log_info "Decrypting with CERT: $tcert, CA: $tca"
                tcmd="socat -u openssl-listen:${TSST_PORT},reuseaddr,cert=${tcert},cafile=${tca}${joiner_extra}${sockopt} stdio"
            else
                wsrep_log_info "Encrypting with CERT: $tcert, CA: $tca"
                tcmd="socat -u stdio openssl-connect:${REMOTEIP}:${TSST_PORT},cert=${tcert},cafile=${tca}${donor_extra}${sockopt}"
            fi
        elif [[ $encrypt -eq 3 ]];then
            wsrep_log_warning "**** WARNING **** encrypt=3 is deprecated and will be removed in a future release"
            wsrep_log_info "Using openssl based encryption with socat: with key and crt"

            verify_file_exists "$tcert" "Both certificate and key files are required." \
                                        "Please check the 'tcert' option.            "
            verify_file_exists "$tkey" "Both certificate and key files are required." \
                                       "Please check the 'tkey' option.             "

            stagemsg+="-OpenSSL-Encrypted-3"
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]];then
                wsrep_log_info "Decrypting with CERT: $tcert, KEY: $tkey"
                tcmd="socat -u openssl-listen:${TSST_PORT},reuseaddr,cert=${tcert},key=${tkey},verify=0${joiner_extra}${sockopt} stdio"
            else
                wsrep_log_info "Encrypting with CERT: $tcert, KEY: $tkey"
                tcmd="socat -u stdio openssl-connect:${REMOTEIP}:${TSST_PORT},cert=${tcert},key=${tkey},verify=0${sockopt}"
            fi
        elif [[ $encrypt -eq 4 ]]; then
            wsrep_log_info "Using openssl based encryption with socat: with key, crt, and ca"

            verify_file_exists "$ssl_ca" "CA, certificate, and key files are required." \
                                         "Please check the 'ssl-ca' option.           "
            verify_file_exists "$ssl_cert" "CA, certificate, and key files are required." \
                                           "Please check the 'ssl-cert' option.         "
            verify_file_exists "$ssl_key" "CA, certificate, and key files are required." \
                                          "Please check the 'ssl-key' option.          "

            # Check to see that the key matches the cert
            verify_cert_matches_key $ssl_cert $ssl_key

            stagemsg+="-OpenSSL-Encrypted-4"
            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]]; then
                wsrep_log_info "Decrypting with CERT: $ssl_cert, KEY: $ssl_key, CA: $ssl_ca"
                tcmd="socat -u openssl-listen:${TSST_PORT},reuseaddr,cert=${ssl_cert},key=${ssl_key},cafile=${ssl_ca},verify=1${joiner_extra}${sockopt} stdio"
            else
                wsrep_log_info "Encrypting with CERT: $ssl_cert, KEY: $ssl_key, CA: $ssl_ca"
                tcmd="socat -u stdio openssl-connect:${REMOTEIP}:${TSST_PORT},cert=${ssl_cert},key=${ssl_key},cafile=${ssl_ca},verify=1${donor_extra}${sockopt}"
            fi

        else
            if [[ $encrypt -eq 1 ]]; then
                wsrep_log_warning "**** WARNING **** encrypt=1 is deprecated and will be removed in a future release"
            fi

            if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]]; then
                tcmd="socat -u TCP-LISTEN:${TSST_PORT},reuseaddr${sockopt} stdio"
            else
                tcmd="socat -u stdio TCP:${REMOTEIP}:${TSST_PORT}${sockopt}"
            fi
        fi
    fi
}

#
# Converts an input string into a boolean "on", "off"
# Converts:
#   "on", "true", "1" -> "on" (case insensitive)
#   "off", "false", "0" -> "off" (case insensitive)
# All other values : -> default_value
#
# 1st param: input value
# 2nd param: default value
normalize_boolean()
{
    local input_value=$1
    local default_value=$2
    local return_value=$default_value

    if [[ "$input_value" == "1" ]]; then
        return_value="on"
    elif [[ "$input_value" =~ ^[Oo][Nn]$ ]]; then
        return_value="on"
    elif [[ "$input_value" =~ ^[Tt][Rr][Uu][Ee]$ ]]; then
        return_value="on"
    elif [[ "$input_value" == "0" ]]; then
        return_value="off"
    elif [[ "$input_value" =~ ^[Oo][Ff][Ff]$ ]]; then
        return_value="off"
    elif [[ "$input_value" =~ ^[Ff][Aa][Ll][Ss][Ee]$ ]]; then
        return_value="off"
    fi

    echo $return_value
}

#
# read the sst specific options.
read_cnf()
{
    sfmt=$(parse_cnf sst streamfmt "xbstream")
    tfmt=$(parse_cnf sst transferfmt "socat")
    tca=$(parse_cnf sst tca "")
    tcert=$(parse_cnf sst tcert "")
    tkey=$(parse_cnf sst tkey "")
    encrypt=$(parse_cnf sst encrypt 0)
    sockopt=$(parse_cnf sst sockopt "")
    progress=$(parse_cnf sst progress "")
    rebuild=$(parse_cnf sst rebuild 0)
    ttime=$(parse_cnf sst time 0)
    cpat=$(parse_cnf sst cpat '.*\.pem$\|.*init\.ok$\|.*galera\.cache$\|.*sst_in_progress$\|.*\.sst$\|.*gvwstate\.dat$\|.*grastate\.dat$\|.*\.err$\|.*\.log$\|.*RPM_UPGRADE_MARKER$\|.*RPM_UPGRADE_HISTORY$')
    ealgo=$(parse_cnf xtrabackup encrypt "")
    ekey=$(parse_cnf xtrabackup encrypt-key "")
    ekeyfile=$(parse_cnf xtrabackup encrypt-key-file "")
    scomp=$(parse_cnf sst compressor "")
    sdecomp=$(parse_cnf sst decompressor "")

    keyring=$(parse_cnf mysqld keyring-file-data "")
    if [[ -z $keyring ]]; then
        keyring=$(parse_cnf sst keyring-file-data "")
    fi

    if [[ -n $keyring ]]; then
        KEYRING_DIR=$(dirname "${keyring}")
    fi

    keyringsid=$(parse_cnf mysqld server-id "")
    if [[ -z $keyringsid ]]; then
        keyringsid=$(parse_cnf sst server-id "")
    fi
    # If we can't find a server-id, assume a default of 0
    if [[ -z $keyringsid ]]; then
        keyringsid=0
    fi

    # Refer to http://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html
    if [[ -z $ealgo ]]; then
        ealgo=$(parse_cnf sst encrypt-algo "")
        ekey=$(parse_cnf sst encrypt-key "")
        ekeyfile=$(parse_cnf sst encrypt-key-file "")
    fi

    # Pull the parameters needed for encrypt=4
    ssl_ca=$(parse_cnf sst ssl-ca "")
    if [[ -z "$ssl_ca" ]]; then
        ssl_ca=$(parse_cnf mysqld ssl-ca "")
    fi
    ssl_cert=$(parse_cnf sst ssl-cert "")
    if [[ -z "$ssl_cert" ]]; then
        ssl_cert=$(parse_cnf mysqld ssl-cert "")
    fi
    ssl_key=$(parse_cnf sst ssl-key "")
    if [[ -z "$ssl_key" ]]; then
        ssl_key=$(parse_cnf mysqld ssl-key "")
    fi

    pxc_encrypt_cluster_traffic=$(parse_cnf mysqld pxc-encrypt-cluster-traffic "")
    pxc_encrypt_cluster_traffic=$(normalize_boolean "$pxc_encrypt_cluster_traffic" "off")

    rlimit=$(parse_cnf sst rlimit "")
    uextra=$(parse_cnf sst use-extra 0)
    iopts=$(parse_cnf sst inno-backup-opts "")
    iapts=$(parse_cnf sst inno-apply-opts "")
    impts=$(parse_cnf sst inno-move-opts "")
    stimeout=$(parse_cnf sst sst-initial-timeout 100)
    ssyslog=$(parse_cnf sst sst-syslog 0)
    ssystag=$(parse_cnf mysqld_safe syslog-tag "${SST_SYSLOG_TAG:-}")
    ssystag+="-"

    if [[ $ssyslog -ne -1 ]]; then
        if $MY_PRINT_DEFAULTS -c $WSREP_SST_OPT_CONF mysqld_safe | tr '_' '-' | grep -q -- "--syslog"; then
            ssyslog=1
        fi
    fi
}

#
# get a feel of how big payload is being used to estimate progress.
get_footprint()
{
    pushd $WSREP_SST_OPT_DATA 1>/dev/null
    payload=$(find . -regex '.*\.ibd$\|.*\.MYI$\|.*\.MYD$\|.*ibdata1$' -type f -print0 | xargs -0 du --block-size=1 -c | awk 'END { print $1 }')
    if $MY_PRINT_DEFAULTS -c $WSREP_SST_OPT_CONF xtrabackup | grep -q -- "--compress"; then
        # QuickLZ has around 50% compression ratio
        # When compression/compaction used, the progress is only an approximate.
        payload=$(( payload*1/2 ))
    fi
    popd 1>/dev/null
    pcmd+=" -s $payload"
    adjust_progress
}

#
# progress emitting function. This is just for user to monitor there is
# no programatic use of this monitoring.
adjust_progress()
{

    if [[ ! -x `which pv` ]]; then
        wsrep_log_error "pv not found in path: $PATH"
        wsrep_log_error "Disabling all progress/rate-limiting"
        pcmd=""
        rlimit=""
        progress=""
        return
    fi

    if [[ -n $progress && $progress != '1' ]]; then
        if [[ -e $progress ]]; then
            pcmd+=" 2>>$progress"
        else
            pcmd+=" 2>$progress"
        fi
    elif [[ -z $progress && -n $rlimit  ]]; then
            # When rlimit is non-zero
            pcmd="pv -q"
    fi

    if [[ -n $rlimit && "$WSREP_SST_OPT_ROLE"  == "donor" ]]; then
        wsrep_log_info "Rate-limiting SST to $rlimit"
        pcmd+=" -L \$rlimit"
    fi
}

#
# data is stream from donor to joiner using xbstream program.
get_stream()
{
    if [[ $sfmt == 'xbstream' ]]; then
        wsrep_log_info "Streaming with xbstream"
        if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]]; then
            strmcmd="xbstream -x"
        else
            strmcmd="xbstream -c \${FILE_TO_STREAM}"
        fi
    else
        sfmt="tar"
        wsrep_log_info "Streaming with tar"
        if [[ "$WSREP_SST_OPT_ROLE"  == "joiner" ]]; then
            strmcmd="tar xfi - "
        else
            strmcmd="tar cf - \${FILE_TO_STREAM} "
        fi

    fi
}

#
# Some more monitoring and cleanup functions.
get_proc()
{
    set +e
    nproc=$(grep -c processor /proc/cpuinfo)
    [[ -z $nproc || $nproc -eq 0 ]] && nproc=1
    set -e
}

sig_joiner_cleanup()
{
    wsrep_log_error "Removing $XB_GTID_INFO_FILE_PATH file due to signal"
    rm -f "$XB_GTID_INFO_FILE_PATH"
    wsrep_log_error "Removing $XB_DONOR_KEYRING_FILE_PATH file due to signal"
    rm -f "$XB_DONOR_KEYRING_FILE_PATH" || true
}

cleanup_joiner()
{
    # Since this is invoked just after exit NNN
    local estatus=$?
    if [[ $estatus -ne 0 ]]; then
        wsrep_log_error "Cleanup after exit with status:$estatus"
    elif [ "${WSREP_SST_OPT_ROLE}" = "joiner" ]; then
        wsrep_log_info "Removing the sst_in_progress file"
        wsrep_cleanup_progress_file
    fi
    if [[ -n $progress && -p $progress ]]; then
        wsrep_log_info "Cleaning up fifo file $progress"
        rm $progress
    fi
    if [[ -n ${STATDIR:-} ]]; then
       [[ -d $STATDIR ]] && rm -rf $STATDIR
    fi

    if [[ -r "${XB_DONOR_KEYRING_FILE_PATH}" ]]; then
        rm -f "${XB_DONOR_KEYRING_FILE_PATH}"
    fi

    # Final cleanup
    pgid=$(ps -o pgid= $$ | grep -o '[0-9]*')

    # This means no setsid done in mysqld.
    # We don't want to kill mysqld here otherwise.
    if [[ $$ -eq $pgid ]]; then

        # This means a signal was delivered to the process.
        # So, more cleanup.
        if [[ $estatus -ge 128 ]]; then
            kill -KILL -$$ || true
        fi

    fi

    exit $estatus
}

check_pid()
{
    local pid_file="$1"
    [ -r "$pid_file" ] && ps -p $(cat "$pid_file") >/dev/null 2>&1
}

cleanup_donor()
{
    # Since this is invoked just after exit NNN
    local estatus=$?
    if [[ $estatus -ne 0 ]]; then
        wsrep_log_error "Cleanup after exit with status:$estatus"
    fi

    if [[ -n ${XTRABACKUP_PID:-} ]]; then
        if check_pid $XTRABACKUP_PID
        then
            wsrep_log_error "xtrabackup process is still running. Killing... "
            kill_xtrabackup
        fi

    fi
    rm -f ${DATA}/${IST_FILE} || true

    if [[ -n $progress && -p $progress ]]; then
        wsrep_log_info "Cleaning up fifo file $progress"
        rm -f $progress || true
    fi

    wsrep_log_info "Cleaning up temporary directories"

    if [[ -n $xtmpdir ]]; then
       [[ -d $xtmpdir ]] &&  rm -rf $xtmpdir || true
    fi

    if [[ -n $itmpdir ]]; then
       [[ -d $itmpdir ]] &&  rm -rf $itmpdir || true
    fi

    # Final cleanup
    pgid=$(ps -o pgid= $$ | grep -o '[0-9]*')

    # This means no setsid done in mysqld.
    # We don't want to kill mysqld here otherwise.
    if [[ $$ -eq $pgid ]]; then

        # This means a signal was delivered to the process.
        # So, more cleanup.
        if [[ $estatus -ge 128 ]]; then
            kill -KILL -$$ || true
        fi

    fi

    rm -rf "${KEYRING_DIR}/${XB_DONOR_KEYRING_FILE}" || true

    exit $estatus
}

kill_xtrabackup()
{
    local PID=$(cat $XTRABACKUP_PID)
    [ -n "$PID" -a "0" != "$PID" ] && kill $PID && (kill $PID && kill -9 $PID) || :
    wsrep_log_info "Removing xtrabackup pid file $XTRABACKUP_PID"
    rm -f "$XTRABACKUP_PID" || true
}

setup_ports()
{
    if [[ "$WSREP_SST_OPT_ROLE"  == "donor" ]]; then
        SST_PORT=$(echo $WSREP_SST_OPT_ADDR | awk -F '[:/]' '{ print $2 }')
        REMOTEIP=$(echo $WSREP_SST_OPT_ADDR | awk -F ':' '{ print $1 }')
        lsn=$(echo $WSREP_SST_OPT_ADDR | awk -F '[:/]' '{ print $4 }')
        sst_ver=$(echo $WSREP_SST_OPT_ADDR | awk -F '[:/]' '{ print $5 }')
    else
        SST_PORT=$(echo ${WSREP_SST_OPT_ADDR} | awk -F ':' '{ print $2 }')
    fi
}

#
# waits ~10 seconds for nc to open the port and then reports ready
# (regardless of timeout)
wait_for_listen()
{
    local PORT=$1
    local ADDR=$2
    local MODULE=$3
    for i in {1..50}
    do
        ss -p state listening "( sport = :$PORT )" | grep -qE 'socat|nc' && break
        sleep 0.2
    done
    echo "ready ${ADDR}/${MODULE}//$sst_ver"
}

#
# check if there are any extra options to parse.
# Note: if port is specified socket is not used.
check_extra()
{
    local use_socket=1
    if [[ $uextra -eq 1 ]]; then
        if $MY_PRINT_DEFAULTS -c $WSREP_SST_OPT_CONF mysqld | tr '_' '-' | grep -- "--thread-handling=" | grep -q 'pool-of-threads'; then
            local eport=$($MY_PRINT_DEFAULTS -c $WSREP_SST_OPT_CONF mysqld | tr '_' '-' | grep -- "--extra-port=" | cut -d= -f2)
            if [[ -n $eport ]]; then
                # Xtrabackup works only locally.
                # Hence, setting host to 127.0.0.1 unconditionally.
                wsrep_log_info "SST through extra_port $eport"
                INNOEXTRA+=" --host=127.0.0.1 --port=$eport "
                use_socket=0
            else
                wsrep_log_error "Extra port $eport null, failing"
                exit 1
            fi
        else
            wsrep_log_info "Thread pool not set, ignore the option use_extra"
        fi
    fi
    if [[ $use_socket -eq 1 ]] && [[ -n "${WSREP_SST_OPT_SOCKET}" ]]; then
        INNOEXTRA+=" --socket=${WSREP_SST_OPT_SOCKET}"
    fi
}

#
# JOINER function to recieve data from DONOR.
#
# 1st param: dir
# 2nd param: msg
# 3rd param: tmt : timeout
# 4th param: checkf : file check
#   If this is -1, skip all error checking
#   If this is 0, do nothing (no additional checks)
#   If this is 1, check to see if the gtid info file exists
#   If this is 2, check to see if the keyring file exists
recv_joiner()
{
    local dir=$1
    local msg=$2
    local tmt=$3
    local checkf=$4
    local ltcmd

    if [[ ! -d ${dir} ]]; then
        # This indicates that IST is in progress
        return
    fi

    pushd ${dir} 1>/dev/null
    set +e

    if [[ $tmt -gt 0 && -x `which timeout` ]]; then
        if timeout --help | grep -q -- '-k'; then
            ltcmd="timeout -k $(( tmt+10 )) $tmt $tcmd"
        else
            ltcmd="timeout -s9 $tmt $tcmd"
        fi
        timeit "$msg" "$ltcmd | $strmcmd; RC=( "\${PIPESTATUS[@]}" )"
    else
        timeit "$msg" "$tcmd | $strmcmd; RC=( "\${PIPESTATUS[@]}" )"
    fi

    set -e
    popd 1>/dev/null

    if [[ $checkf -eq -1 ]]; then
        # we don't care about errors (or we expect an error to occur)
        # just return
        return
    fi

    if [[ ${RC[0]} -eq 124 ]]; then
        wsrep_log_error "Possible timeout in receving first data from donor in "
                        "gtid/keyring stage"
        exit 32
    fi

    for ecode in "${RC[@]}";do
        if [[ $ecode -ne 0 ]]; then
            wsrep_log_error "Error while getting data from donor node: " \
                            "exit codes: ${RC[@]}"
            exit 32
        fi
    done

    if [[ $checkf -eq 1 && ! -r "${XB_GTID_INFO_FILE_PATH}" ]]; then
        # this message should cause joiner to abort
        wsrep_log_error "xtrabackup process ended without creating '${XB_GTID_INFO_FILE_PATH}'"
        wsrep_log_info "Contents of datadir"
        wsrep_log_info "$(ls -l ${dir}/*)"
        exit 32
    fi

    if [[ $checkf -eq 2 && ! -r "${XB_DONOR_KEYRING_FILE_PATH}" ]]; then
        wsrep_log_error "FATAL: xtrabackup could not find '${XB_DONOR_KEYRING_FILE_PATH}'"
        wsrep_log_error "The joiner is using a keyring file but the donor has not sent"
        wsrep_log_error "a keyring file.  Please check your configuration to ensure that"
        wsrep_log_error "both sides are using a keyring file"
        exit 32
    fi

}

#
# Process to send data from DONOR to JOINER
send_donor()
{
    local dir=$1
    local msg=$2

    pushd ${dir} 1>/dev/null
    set +e
    timeit "$msg" "$strmcmd | $tcmd; RC=( "\${PIPESTATUS[@]}" )"
    set -e
    popd 1>/dev/null


    for ecode in "${RC[@]}";do
        if [[ $ecode -ne 0 ]]; then
            wsrep_log_error "Error while sending data to joiner node: " \
                            "exit codes: ${RC[@]}"
            exit 32
        fi
    done
}

# Returns the version string in a standardized format
# Input "1.2.3" => echoes "010203"
# Wrongly formatted values => echoes "000000"
normalize_version()
{
    local major=0
    local minor=0
    local patch=0

    # Only parses purely numeric version numbers, 1.2.3 
    # Everything after the first three values are ignored
    if [[ $1 =~ ^([0-9]+)\.([0-9]+)\.?([0-9]*)([\.0-9])*$ ]]; then
        major=${BASH_REMATCH[1]}
        minor=${BASH_REMATCH[2]}
        patch=${BASH_REMATCH[3]}
    fi

    printf %02d%02d%02d $major $minor $patch
}

# Compares two version strings
# The first parameter is the version to be checked
# The second parameter is the minimum version required
# Returns 1 (failure) if $1 >= $2, 0 (success) otherwise
check_for_version()
{
    local local_version_str="$( normalize_version $1 )"
    local required_version_str="$( normalize_version $2 )"

    if [[ "$local_version_str" < "$required_version_str" ]]; then
        return 1
    else
        return 0
    fi
}


#-------------------------------------------------------------------------------
#
# Step-4: Main workload logic starts here.
#

#
# Validation check for xtrabackup binary.
if [[ ! -x `which $INNOBACKUPEX_BIN` ]]; then
    wsrep_log_error "$INNOBACKUPEX_BIN not in path: $PATH"
    exit 2
fi

# XB_REQUIRED_VERSION requires at least major.minor version (e.g. 2.4.1 or 3.0)
#
# 2.4.3 : Starting with 5.7, the redo log format has changed and so XB-2.4.3 or higher is needed
# for performing backup (read SST)
#
# 2.4.4 : needed to support the keyring option
#
XB_REQUIRED_VERSION="2.4.4"

XB_VERSION=`$INNOBACKUPEX_BIN --version 2>&1 | grep -oe '[0-9]\.[0-9][\.0-9]*' | head -n1`
if [[ -z $XB_VERSION ]]; then
    wsrep_log_error "Cannot determine the $INNOBACKUPEX_BIN version. Needs xtrabackup-$XB_REQUIRED_VERSION or higher to perform SST"
    exit 2
fi

if ! check_for_version $XB_VERSION $XB_REQUIRED_VERSION; then
    wsrep_log_error "The $INNOBACKUPEX_BIN version is $XB_VERSION. Needs xtrabackup-$XB_REQUIRED_VERSION or higher to perform SST"
    exit 2
fi

wsrep_log_info "The $INNOBACKUPEX_BIN version is $XB_VERSION"

rm -f "${XB_GTID_INFO_FILE_PATH}"

#
# establish roles. Either it should be JOINER or DONOR.
if [[ ! ${WSREP_SST_OPT_ROLE} == 'joiner' && ! ${WSREP_SST_OPT_ROLE} == 'donor' ]]; then
    wsrep_log_error "Invalid role ${WSREP_SST_OPT_ROLE}"
    exit 22
fi

#
# read configuration and setup ports for streaming data.
read_cnf
setup_ports

#
# If pxc_encrypt_cluster_traffic is set, do the SSL autoconfig
# (overriding any values already in the config file)
if [[ "$pxc_encrypt_cluster_traffic" == "on" ]]; then
    encrypt=4

    # Look for the [mysqld] files only, not the ones in [sst]
    ssl_ca=$(parse_cnf mysqld ssl-ca "")
    ssl_cert=$(parse_cnf mysqld ssl-cert "")
    ssl_key=$(parse_cnf mysqld ssl-key "")

    # Check that we have all the files
    # If they have not been explicitly specified, check the datadir
    if [[ -z "$ssl_ca" ]]; then
        if [[ -r "$DATA/ca.pem" ]]; then
            ssl_ca="$DATA/ca.pem"
        else
            wsrep_log_error "******** FATAL ERROR ******************************* "
            wsrep_log_error "* Could not find a CA (Certificate Authority) file.  "
            wsrep_log_error "* Please specify a CA file with the 'ssl-ca' option. "
            wsrep_log_error "**************************************************** "
            return 2
        fi
    fi
    if [[ -z "$ssl_cert" ]]; then
        if [[ -r "$DATA/server-cert.pem" ]]; then
            ssl_cert="$DATA/server-cert.pem"
        else
            wsrep_log_error "******** FATAL ERROR ****************************************** "
            wsrep_log_error "* Could not find a certificate file.                            "
            wsrep_log_error "* Please specify a certificate file with the 'ssl-cert' option. "
            wsrep_log_error "*************************************************************** "
            return 2
        fi
    fi
    if [[ -z "$ssl_key" ]]; then
        if [[ -r "$DATA/server-key.pem" ]]; then
            ssl_key="$DATA/server-key.pem"
        else
            wsrep_log_error "******** FATAL ERROR ********************************* "
            wsrep_log_error "* Could not find a key file.                           "
            wsrep_log_error "* Please specify a key file with the 'ssl-key' option. "
            wsrep_log_error "****************************************************** "
            return 2
        fi
    fi

    wsrep_log_info "pxc_encrypt_cluster_traffic is enabled, using PXC auto-ssl configuration"
    wsrep_log_info "with encrypt=4  ssl_ca=$ssl_ca  ssl_cert=$ssl_cert  ssl_key=$ssl_key"
fi

if ${INNOBACKUPEX_BIN} /tmp --help 2>/dev/null | grep -q -- '--version-check'; then
    disver="--no-version-check"
fi

if [[ ${FORCE_FTWRL:-0} -eq 1 ]]; then
    wsrep_log_info "Forcing FTWRL due to environment variable FORCE_FTWRL equal to $FORCE_FTWRL"
    iopts+=" --no-backup-locks "
fi

INNOEXTRA=""

#
# Use syslogger (for logging) if configured else continue
# to use file-based logging.
if [[ $ssyslog -eq 1 ]]; then

    if [[ ! -x `which logger` ]]; then
        wsrep_log_error "logger not in path: $PATH. Ignoring"
    else

        wsrep_log_info "Logging all stderr of SST/XtraBackup to syslog"

        exec 2> >(logger -p daemon.err -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE)

        wsrep_log_error()
        {
            logger -p daemon.err -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE "$@"
        }

        wsrep_log_info()
        {
            logger -p daemon.info -t ${ssystag}wsrep-sst-$WSREP_SST_OPT_ROLE "$@"
        }

        INNOAPPLY="${INNOBACKUPEX_BIN} $disver $iapts --prepare --binlog-info=ON \$rebuildcmd \$keyringapplyopt --target-dir=\${DATA} 2>&1  | logger -p daemon.err -t ${ssystag}innobackupex-apply "
        INNOMOVE="${INNOBACKUPEX_BIN} --defaults-file=${WSREP_SST_OPT_CONF} --datadir=\${TDATA} $disver $impts  --move-back --binlog-info=ON --force-non-empty-directories --target-dir=\${DATA} 2>&1 | logger -p daemon.err -t ${ssystag}innobackupex-move "
        INNOBACKUP="${INNOBACKUPEX_BIN} --defaults-file=${WSREP_SST_OPT_CONF} $disver $iopts \$tmpopts \$INNOEXTRA \$keyringbackupopt --backup --galera-info  --binlog-info=ON --stream=\$sfmt --target-dir=\$itmpdir 2> >(logger -p daemon.err -t ${ssystag}innobackupex-backup)"
    fi
else
    INNOAPPLY="${INNOBACKUPEX_BIN} $disver $iapts --prepare --binlog-info=ON \$rebuildcmd \$keyringapplyopt --target-dir=\${DATA} &>\${DATA}/innobackup.prepare.log"
    INNOMOVE="${INNOBACKUPEX_BIN} --defaults-file=${WSREP_SST_OPT_CONF}  --defaults-group=mysqld${WSREP_SST_OPT_CONF_SUFFIX} --datadir=\${TDATA} $disver $impts  --move-back --binlog-info=ON --force-non-empty-directories --target-dir=\${DATA} &>\${DATA}/innobackup.move.log"
    INNOBACKUP="${INNOBACKUPEX_BIN} --defaults-file=${WSREP_SST_OPT_CONF}  --defaults-group=mysqld${WSREP_SST_OPT_CONF_SUFFIX} $disver $iopts \$tmpopts \$INNOEXTRA \$keyringbackupopt --backup --galera-info --binlog-info=ON --stream=\$sfmt --target-dir=\$itmpdir 2>\${DATA}/innobackup.backup.log"
fi

#
# Setup stream for transfering and streaming.
get_stream
get_transfer

if [ "$WSREP_SST_OPT_ROLE" = "donor" ]
then
    #
    # signal handler for cleanup-based-exit.
    trap cleanup_donor EXIT

    #
    # SST is not needed. IST would suffice. By-pass SST.
    if [ $WSREP_SST_OPT_BYPASS -eq 0 ]
    then
        usrst=0
        if [[ -z $sst_ver ]]; then
            wsrep_log_error "Upgrade joiner to 5.6.21 or higher for backup locks support"
            wsrep_log_error "The joiner is not supported for this version of donor"
            exit 93
        fi

        if [[ -z $(parse_cnf mysqld tmpdir "") && -z $(parse_cnf xtrabackup tmpdir "") ]]; then
            xtmpdir=$(mktemp -d)
            tmpopts=" --tmpdir=$xtmpdir "
            wsrep_log_info "Using $xtmpdir as xtrabackup temporary directory"
        fi

        itmpdir=$(mktemp -d)
        wsrep_log_info "Using $itmpdir as innobackupex temporary directory"

        if [[ -n "${WSREP_SST_OPT_USER:-}" && "$WSREP_SST_OPT_USER" != "(null)" ]]; then
           INNOEXTRA+=" --user=$WSREP_SST_OPT_USER"
           usrst=1
        fi

        if [ -n "${WSREP_SST_OPT_PSWD:-}" ]; then
           INNOEXTRA+=" --password=$WSREP_SST_OPT_PSWD"
        elif [[ $usrst -eq 1 ]]; then
           # Empty password, used for testing, debugging etc.
           INNOEXTRA+=" --password="
        fi

        get_keys
        check_extra

        ttcmd="$tcmd"
        # add encryption onto the head of the stream
        if [[ $encrypt -eq 1 ]]; then
            tcmd=" \$ecmd | $tcmd "
        fi
        # add compression onto the head of the stream
        if [[ -n $scomp ]]; then
            tcmd=" $scomp | $tcmd "
        fi


        wsrep_log_info "Streaming GTID file before SST"
        echo "${WSREP_SST_OPT_GTID}" > "${XB_GTID_INFO_FILE_PATH}"
        FILE_TO_STREAM=$XB_GTID_INFO_FILE
        send_donor $DATA "${stagemsg}-gtid"

        if [[ -n $keyring ]]; then
            # Verify that encryption is being used
            # Do NOT send a keyring file without encryption
            # Need encrypt >= 1
            if [[ $encrypt -le 0 ]]; then
                wsrep_log_error "FATAL: An unencrypted channel is being used."
                wsrep_log_error "Sending/using a keyring requires an encrypted channel."
                exit 22
            fi

            # joiner need to wait to receive the file.
            sleep 3

            cp $keyring $KEYRING_DIR/$XB_DONOR_KEYRING_FILE

            wsrep_log_info "Streaming donor-keyring file before SST"
            keyringbackupopt=" --keyring-file-data=${KEYRING_DIR}/${XB_DONOR_KEYRING_FILE} --server-id=$keyringsid "
            FILE_TO_STREAM=$XB_DONOR_KEYRING_FILE
            send_donor "${KEYRING_DIR}" "${stagemsg}-keyring"

        fi

        # Restore the transport commmand to its original state
        tcmd="$ttcmd"
        if [[ -n $progress ]]; then
            get_footprint
            tcmd="$pcmd | $tcmd"
        elif [[ -n $rlimit ]]; then
            adjust_progress
            tcmd="$pcmd | $tcmd"
        fi

        wsrep_log_info "Sleeping before data transfer for SST"
        sleep 10

        wsrep_log_info "Streaming the backup to joiner at ${REMOTEIP} ${SST_PORT:-4444}"

        # add encryption onto the head of the stream
        if [[ $encrypt -eq 1 ]]; then
            tcmd=" \$ecmd | $tcmd "
        fi

        # add compression onto the head of the stream
        if [[ -n $scomp ]]; then
            tcmd=" $scomp | $tcmd "
        fi

        set +e
        timeit "${stagemsg}-SST" "$INNOBACKUP | $tcmd; RC=( "\${PIPESTATUS[@]}" )"
        set -e

        if [ ${RC[0]} -ne 0 ]; then
            wsrep_log_error "${INNOBACKUPEX_BIN} finished with error: ${RC[0]}. " \
                            "Check ${DATA}/innobackup.backup.log"
            exit 22
        elif [[ ${RC[$(( ${#RC[@]}-1 ))]} -eq 1 ]]; then
            wsrep_log_error "$tcmd finished with error: ${RC[1]}"
            exit 22
        fi

        # innobackupex implicitly writes PID to fixed location in $xtmpdir
        XTRABACKUP_PID="$xtmpdir/xtrabackup_pid"

    else # BYPASS FOR IST

        wsrep_log_info "Bypassing the SST for IST"
        echo "continue" # now server can resume updating data
        echo "${WSREP_SST_OPT_GTID}" > "${XB_GTID_INFO_FILE_PATH}"
        echo "1" > "${DATA}/${IST_FILE}"

        get_keys
        # add encryption onto the head of the stream
        if [[ $encrypt -eq 1 ]]; then
            tcmd=" \$ecmd | $tcmd "
        fi
        # add compression onto the head of the stream
        if [[ -n $scomp ]]; then
            tcmd=" $scomp | $tcmd "
        fi
        strmcmd+=" \${IST_FILE}"

	FILE_TO_STREAM=$XB_GTID_INFO_FILE
        send_donor $DATA "${stagemsg}-IST"

    fi

    echo "done ${WSREP_SST_OPT_GTID}"
    wsrep_log_info "Total time on donor: $totime seconds"

elif [ "${WSREP_SST_OPT_ROLE}" = "joiner" ]
then

    [[ -e $SST_PROGRESS_FILE ]] && wsrep_log_info "Stale sst_in_progress file: $SST_PROGRESS_FILE"
    [[ -n $SST_PROGRESS_FILE ]] && touch $SST_PROGRESS_FILE

    ib_home_dir=$(parse_cnf mysqld innodb-data-home-dir "")
    ib_log_dir=$(parse_cnf mysqld innodb-log-group-home-dir "")
    ib_undo_dir=$(parse_cnf mysqld innodb-undo-directory "")

    stagemsg="Joiner-Recv"

    nthreads=1

    MODULE="xtrabackup_sst"

    rm -f "${DATA}/${IST_FILE}"

    # May need xtrabackup_checkpoints later on
    rm -f ${DATA}/xtrabackup_binary ${DATA}/xtrabackup_galera_info  ${DATA}/xtrabackup_logfile
    if [[ -n $KEYRING_DIR ]]; then
        rm -f "${KEYRING_DIR}/${XB_DONOR_KEYRING_FILE}"
    fi

    ADDR=${WSREP_SST_OPT_ADDR}
    if [ -z "${SST_PORT}" ]
    then
        SST_PORT=4444
        ADDR="$(echo ${WSREP_SST_OPT_ADDR} | awk -F ':' '{ print $1 }'):${SST_PORT}"
    fi

    wait_for_listen ${SST_PORT} ${ADDR} ${MODULE} &

    trap sig_joiner_cleanup HUP PIPE INT TERM
    trap cleanup_joiner EXIT

    if [[ -n $progress ]]; then
        adjust_progress
        tcmd+=" | $pcmd"
    fi

    get_keys
    if [[ -n $sdecomp ]]; then
        strmcmd=" $sdecomp | $strmcmd"
    fi
    if [[ $encrypt -eq 1 ]]; then
        strmcmd=" \$ecmd | $strmcmd"
    fi

    STATDIR=$(mktemp -d)
    XB_GTID_INFO_FILE_PATH="${STATDIR}/${XB_GTID_INFO_FILE}"
    recv_joiner $STATDIR "${stagemsg}-gtid" $stimeout 1

    # server-id is already part of backup-my.cnf so avoid appending it.
    # server-id is the id of the node that is acting as donor and not joiner node.

    if [[ -n $keyring ]]; then
        # joiner needs to wait to receive the file.
        sleep 3

        XB_DONOR_KEYRING_FILE_PATH="${KEYRING_DIR}/${XB_DONOR_KEYRING_FILE}"
        recv_joiner "${KEYRING_DIR}" "${stagemsg}-keyring" 0 2
        keyringapplyopt=" --keyring-file-data=$XB_DONOR_KEYRING_FILE_PATH "

        wsrep_log_info "donor keyring received at: '${XB_DONOR_KEYRING_FILE_PATH}'"
    else
        # There shouldn't be a keyring file, since we do not have a keyring here
        # This will error out if a keyring file IS found
        XB_DONOR_KEYRING_FILE_PATH="${KEYRING_DIR}/${XB_DONOR_KEYRING_FILE}"
        recv_joiner "${KEYRING_DIR}" "${stagemsg}-keyring" 5 -1

        if [[ -r "${XB_DONOR_KEYRING_FILE_PATH}" ]]; then
            wsrep_log_error "FATAL: xtrabackup found '${XB_DONOR_KEYRING_FILE_PATH}'"
            wsrep_log_error "The joiner is not using a keyring file but the donor has sent"
            wsrep_log_error "a keyring file.  Please check your configuration to ensure that"
            wsrep_log_error "both sides are using a keyring file"
            exit 32
        fi
    fi

    if ! ps -p ${WSREP_SST_OPT_PARENT} &>/dev/null
    then
        wsrep_log_error "Parent mysqld process (PID:${WSREP_SST_OPT_PARENT}) terminated unexpectedly."
        exit 32
    fi

    if [ ! -r "${STATDIR}/${IST_FILE}" ]
    then
        if [[ -d ${DATA}/.sst ]]; then
            wsrep_log_info "WARNING: Stale temporary SST directory: ${DATA}/.sst from previous state transfer. Removing"
            rm -rf ${DATA}/.sst
        fi
        mkdir -p ${DATA}/.sst
        (recv_joiner $DATA/.sst "${stagemsg}-SST" 0 0) &
        jpid=$!
        wsrep_log_info "Proceeding with SST"

        wsrep_log_info "Cleaning the existing datadir and innodb-data/log directories"
        # Avoid emitting the find command output to log file. It just fill the
        # with ever increasing number of files and achieve nothing.
        find $ib_home_dir $ib_log_dir $ib_undo_dir $DATA -mindepth 1  -regex $cpat  -prune  -o -exec rm -rfv {} 1>/dev/null \+

        tempdir=$(parse_cnf mysqld log-bin "")
        if [[ -n ${tempdir:-} ]]; then
            binlog_dir=$(dirname $tempdir)
            binlog_file=$(basename $tempdir)
            if [[ -n ${binlog_dir:-} && $binlog_dir != '.' && $binlog_dir != $DATA ]]; then
                pattern="$binlog_dir/$binlog_file\.[0-9]+$"
                wsrep_log_info "Cleaning the binlog directory $binlog_dir as well"
                find $binlog_dir -maxdepth 1 -type f -regex $pattern -exec rm -fv {} 1>&2 \+ || true
                rm $binlog_dir/*.index || true
            fi
        fi

        TDATA=${DATA}
        DATA="${DATA}/.sst"

        XB_GTID_INFO_FILE_PATH="${DATA}/${XB_GTID_INFO_FILE}"
        wsrep_log_info "Waiting for SST streaming to complete!"
        wait $jpid

        get_proc

        if [[ ! -s ${DATA}/xtrabackup_checkpoints ]]; then
            wsrep_log_error "xtrabackup_checkpoints missing, failed xtrabackup/SST on donor"
            exit 2
        fi

        # Rebuild indexes for compact backups
        if grep -q 'compact = 1' ${DATA}/xtrabackup_checkpoints; then
            wsrep_log_info "Index compaction detected"
            rebuild=1
        fi

        if [[ $rebuild -eq 1 ]]; then
            nthreads=$(parse_cnf xtrabackup rebuild-threads $nproc)
            wsrep_log_info "Rebuilding during prepare with $nthreads threads"
            rebuildcmd="--rebuild-indexes --rebuild-threads=$nthreads"
        fi

        if test -n "$(find ${DATA} -maxdepth 1 -type f -name '*.qp' -print -quit)"; then

            wsrep_log_info "Compressed qpress files found"

            if [[ ! -x `which qpress` ]]; then
                wsrep_log_error "qpress not found in path: $PATH"
                exit 22
            fi

            if [[ -n $progress ]] && pv --help | grep -q 'line-mode'; then
                count=$(find ${DATA} -type f -name '*.qp' | wc -l)
                count=$(( count*2 ))
                if pv --help | grep -q FORMAT; then
                    pvopts="-f -s $count -l -N Decompression -F '%N => Rate:%r Elapsed:%t %e Progress: [%b/$count]'"
                else
                    pvopts="-f -s $count -l -N Decompression"
                fi
                pcmd="pv $pvopts"
                adjust_progress
                dcmd="$pcmd | xargs -n 2 qpress -T${nproc}d"
            else
                dcmd="xargs -n 2 qpress -T${nproc}d"
            fi


            # Decompress the qpress files
            wsrep_log_info "Decompression with $nproc threads"
            timeit "Joiner-Decompression" "find ${DATA} -type f -name '*.qp' -printf '%p\n%h\n' | $dcmd"
            extcode=$?

            if [[ $extcode -eq 0 ]]; then
                wsrep_log_info "Removing qpress files after decompression"
                find ${DATA} -type f -name '*.qp' -delete
                if [[ $? -ne 0 ]]; then
                    wsrep_log_error "Something went wrong with deletion of qpress files. Investigate"
                fi
            else
                wsrep_log_error "Decompression failed. Exit code: $extcode"
                exit 22
            fi
        fi

        if  [[ ! -z $WSREP_SST_OPT_BINLOG ]]; then

            BINLOG_DIRNAME=$(dirname $WSREP_SST_OPT_BINLOG)
            BINLOG_FILENAME=$(basename $WSREP_SST_OPT_BINLOG)

            # To avoid comparing data directory and BINLOG_DIRNAME
            mv $DATA/${BINLOG_FILENAME}.* $BINLOG_DIRNAME/ 2>/dev/null || true

            pushd $BINLOG_DIRNAME &>/dev/null
            for bfiles in $(ls -1 ${BINLOG_FILENAME}.*);do
                echo ${BINLOG_DIRNAME}/${bfiles} >> ${BINLOG_FILENAME}.index
            done
            popd &> /dev/null

        fi

        wsrep_log_info "Preparing the backup at ${DATA}"
        timeit "Xtrabackup prepare stage" "$INNOAPPLY"

        if [ $? -ne 0 ];
        then
            wsrep_log_error "${INNOBACKUPEX_BIN} apply finished with errors. Check ${DATA}/innobackup.prepare.log"
            exit 22
        fi

        XB_GTID_INFO_FILE_PATH="${TDATA}/${XB_GTID_INFO_FILE}"
        set +e
        rm $TDATA/innobackup.prepare.log $TDATA/innobackup.move.log
        set -e
        wsrep_log_info "Moving the backup to ${TDATA}"
        timeit "Xtrabackup move stage" "$INNOMOVE"
        if [[ $? -eq 0 ]]; then

            # Did we receive a keyring file?
            if [[ -r "${XB_DONOR_KEYRING_FILE_PATH}" ]]; then
                wsrep_log_info "Moving sst keyring into place: moving $XB_DONOR_KEYRING_FILE_PATH to $keyring"
                mv "${XB_DONOR_KEYRING_FILE_PATH}" $keyring
                wsrep_log_info "Keyring move successful"
            fi

            wsrep_log_info "Move successful, removing ${DATA}"
            rm -rf $DATA
            DATA=${TDATA}
        else
            if [[ -r "${XB_DONOR_KEYRING_FILE_PATH}" ]]; then
                rm -f "${XB_DONOR_KEYRING_FILE_PATH}"
            fi
            wsrep_log_error "Move failed, keeping ${DATA} for further diagnosis"
            wsrep_log_error "Check ${DATA}/innobackup.move.log for details"
            exit 22
        fi

    else
        wsrep_log_info "${IST_FILE} received from donor: Running IST"
    fi

    if [[ ! -r ${XB_GTID_INFO_FILE_PATH} ]]; then
        wsrep_log_error "SST magic file ${XB_GTID_INFO_FILE_PATH} not found/readable"
        exit 2
    fi
    wsrep_log_info "Galera co-ords from recovery: $(cat ${XB_GTID_INFO_FILE_PATH})"
    cat "${XB_GTID_INFO_FILE_PATH}" # output UUID:seqno
    wsrep_log_info "Total time on joiner: $totime seconds"
fi

exit 0
