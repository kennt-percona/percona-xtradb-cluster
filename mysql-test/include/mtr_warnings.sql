-- Copyright (c) 2008, 2019, Oracle and/or its affiliates. All rights reserved.
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License, version 2.0,
-- as published by the Free Software Foundation.
--
-- This program is also distributed with certain software (including
-- but not limited to OpenSSL) that is licensed under separate terms,
-- as designated in a particular file or component or in included license
-- documentation.  The authors of MySQL hereby grant you an additional
-- permission to link the program and your derivative works with the
-- separately licensed software that they have included with MySQL.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License, version 2.0, for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software Foundation,
-- 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA

delimiter ||;

use mtr||

--
-- Create table where testcases can insert patterns to
-- be suppressed
--
CREATE TABLE test_suppressions (
  pattern VARCHAR(255)
) ENGINE=MyISAM||


--
-- Declare a trigger that makes sure
-- no invalid patterns can be inserted
-- into test_suppressions
--
SET @character_set_client_saved = @@character_set_client||
SET @character_set_results_saved = @@character_set_results||
SET @collation_connection_saved = @@collation_connection||
SET @@character_set_client = latin1||
SET @@character_set_results = latin1||
SET @@collation_connection = latin1_swedish_ci||
/*!50002
CREATE DEFINER=root@localhost TRIGGER ts_insert
BEFORE INSERT ON test_suppressions
FOR EACH ROW BEGIN
  DECLARE dummy INT;
  SELECT "" REGEXP NEW.pattern INTO dummy;
END
*/||
SET @@character_set_client = @character_set_client_saved||
SET @@character_set_results = @character_set_results_saved||
SET @@collation_connection = @collation_connection_saved||


--
-- Load table with patterns that will be suppressed globally(always)
--
CREATE TABLE global_suppressions (
  pattern VARCHAR(255)
) ENGINE=MyISAM||


-- Declare a trigger that makes sure
-- no invalid patterns can be inserted
-- into global_suppressions
--
SET @character_set_client_saved = @@character_set_client||
SET @character_set_results_saved = @@character_set_results||
SET @collation_connection_saved = @@collation_connection||
SET @@character_set_client = latin1||
SET @@character_set_results = latin1||
SET @@collation_connection = latin1_swedish_ci||
/*!50002
CREATE DEFINER=root@localhost TRIGGER gs_insert
BEFORE INSERT ON global_suppressions
FOR EACH ROW BEGIN
  DECLARE dummy INT;
  SELECT "" REGEXP NEW.pattern INTO dummy;
END
*/||
SET @@character_set_client = @character_set_client_saved||
SET @@character_set_results = @character_set_results_saved||
SET @@collation_connection = @collation_connection_saved||



--
-- Insert patterns that should always be suppressed
--
INSERT INTO global_suppressions VALUES
 (".SELECT UNIX_TIMESTAMP... failed on master"),
 ("Aborted connection"),
 ("Client requested master to start replication from position"),
 ("Could not find first log file name in binary log"),
 ("Enabling keys got errno"),
 ("Error reading master configuration"),
 ("Error reading packet"),
 ("Event Scheduler"),
 ("Failed to open log"),
 ("Failed to open the existing master info file"),
 ("Forcing shutdown of [0-9]* plugins"),
 ("Forcing close of thread"),

 ("Percona Server cannot operate under OpenSSL FIPS mode"),
 /*
   Due to timing issues, it might be that this warning
   is printed when the server shuts down and the
   computer is loaded.
 */

 ("Got error [0-9]* when reading table"),
 ("Incorrect definition of table"),
 ("Incorrect information in file"),
 ("InnoDB: Warning: we did not need to do crash recovery"),
 ("Invalid \\(old\\?\\) table or database name"),
 ("Lock wait timeout exceeded"),
 ("Log entry on master is longer than max_allowed_packet"),
 ("unknown option '--loose-"),
 ("unknown variable 'loose-"),
 ("You have forced lower_case_table_names to 0 through a command-line option"),
 ("Setting lower_case_table_names=2"),
 ("NDB Binlog:"),
 ("NDB: failed to setup table"),
 ("NDB: only row based binary logging"),
 ("Neither --relay-log nor --relay-log-index were used"),
 ("Query partially completed"),
 ("Slave I.O thread aborted while waiting for relay log"),
 ("Slave SQL thread is stopped because UNTIL condition"),
 ("Slave SQL thread retried transaction"),
 ("Slave \\(additional info\\)"),
 ("Slave: .*Duplicate column name"),
 ("Slave: .*master may suffer from"),
 ("Slave: According to the master's version"),
 ("Slave: Column [0-9]* type mismatch"),
 ("Slave: Error .* doesn't exist"),
 ("Slave: Error .*Unknown table"),
 ("Slave: Error in Write_rows event: "),
 ("Slave: Field .* of table .* has no default value"),
 ("Slave: Field .* doesn't have a default value"),
 ("Slave: Query caused different errors on master and slave"),
 ("Slave: Table .* doesn't exist"),
 ("Slave: Table width mismatch"),
 ("Slave: The incident LOST_EVENTS occured on the master"),
 ("Slave: Unknown error.* 1105"),
 ("Slave: Can't drop database.* database doesn't exist"),
 ("Sort aborted"),
 ("Time-out in NDB"),
 ("Warning:\s+One can only use the --user.*root"),
 ("Warning:\s+Table:.* on (delete|rename)"),
 ("You have an error in your SQL syntax"),
 ("deprecated"),
 ("description of time zone"),
 ("equal MySQL server ids"),
 ("error .*connecting to master"),
 ("error reading log entry"),
 ("lower_case_table_names is set"),
 ("skip-name-resolve mode"),
 ("slave SQL thread aborted"),
 ("Slave: .*Duplicate entry"),

 ("Statement may not be safe to log in statement format"),

 /* innodb foreign key tests that fail in ALTER or RENAME produce this */
 ("InnoDB: Error: in ALTER TABLE `test`.`t[123]`"),
 ("InnoDB: Error: in RENAME TABLE table `test`.`t1`"),
 ("InnoDB: Error: table `test`.`t[123]` does not exist in the InnoDB internal"),

 /*
   BUG#32080 - Excessive warnings on Solaris: setrlimit could not
   change the size of core files
  */
 ("setrlimit could not change the size of core files to 'infinity'"),

 ("The slave I.O thread stops because a fatal error is encountered when it tries to get the value of SERVER_UUID variable from master.*"),
 ("The initialization command '.*' failed with the following error.*"),

 /*It will print a warning if a new UUID of server is generated.*/
 ("No existing UUID has been found, so we assume that this is the first time that this server has been started.*"),
 /*It will print a warning if server is run without --explicit_defaults_for_timestamp.*/
 ("TIMESTAMP with implicit DEFAULT value is deprecated. Please use --explicit_defaults_for_timestamp server option (see documentation for more details)*"),

 /* Added 2009-08-XX after fixing Bug #42408 */

 ("Although a path was specified for the --general-log-file option, log tables are used"),
 ("Although a path was specified for the --slow-query-log-file option, log tables are used"),
 ("Backup: Operation aborted"),
 ("Restore: Operation aborted"),
 ("Restore: The grant .* was skipped because the user does not exist"),
 ("The path specified for the variable .* is not a directory or cannot be written:"),
 ("Master server does not support or not configured semi-sync replication, fallback to asynchronous"),
 (": The MySQL server is running with the --secure-backup-file-priv option so it cannot execute this statement"),
 ("Slave: Unknown table 'test.t1' Error_code: 1051"),

 /* Messages from valgrind */
 ("==[0-9]*== Memcheck,"),
 ("==[0-9]*== Copyright"),
 ("==[0-9]*== Using"),
 ("==[0-9]*== For more details"),
 /* This comes with innodb plugin tests */
 ("==[0-9]*== Warning: set address range perms: large range"),
 /* valgrind-3.5.0 dumps this */
 ("==[0-9]*== Command: "),

 /* valgrind warnings: invalid file descriptor -1 in syscall
    write()/read(). Bug #50414 */
 ("==[0-9]*== Warning: invalid file descriptor -1 in syscall write()"),
 ("==[0-9]*== Warning: invalid file descriptor -1 in syscall read()"),

 /*
   Transient network failures that cause warnings on reconnect.
   BUG#47743 and BUG#47983.
 */
 ("Slave I/O: Get master SERVER_UUID failed with error:.*"),
 ("Slave I/O: Get master SERVER_ID failed with error:.*"),
 ("Slave I/O: Get master clock failed with error:.*"),
 ("Slave I/O: Get master COLLATION_SERVER failed with error:.*"),
 ("Slave I/O: Get master TIME_ZONE failed with error:.*"),
 ("Slave I/O: The slave I/O thread stops because a fatal error is encountered when it tried to SET @master_binlog_checksum on master.*"),
 ("Slave I/O: Get master BINLOG_CHECKSUM failed with error.*"),
 ("Slave I/O: Notifying master by SET @master_binlog_checksum= @@global.binlog_checksum failed with error.*"),
 /*
   BUG#42147 - Concurrent DML and LOCK TABLE ... READ for InnoDB 
   table cause warnings in errlog
   Note: This is a temporary suppression until Bug#42147 can be 
   fixed properly. See bug page for more information.
  */
 ("Found lock of type 6 that is write and read locked"),

 /*
   Warning message is printed out whenever a slave is started with
   a configuration that is not crash-safe.
 */
 (".*If a crash happens this configuration does not guarantee.*"),

 /*
   Warning messages introduced in the context of the WL#4143.
 */
 ("Storing MySQL user name or password information in the master.info repository is not secure.*"),
 ("Sending passwords in plain text without SSL/TLS is extremely insecure."),

 /*
  In MTS if the user issues a stop slave sql while it is scheduling a group
  of events, this warning is emitted.
  */
 ("Slave SQL: Coordinator thread of multi-threaded slave is being stopped in the middle of assigning a group of events.*"),
 
 ("Changed limits: max_open_files: *"),
 ("Changed limits: max_connections: *"),
 ("Changed limits: table_open_cache: *"),
 ("Could not increase number of max_open_files to more than *"),

 /*
  Warnings related to --secure-file-priv
 */
 ("Insecure configuration for --secure-file-priv:*"),

 /*
  Bug#26585560, warning related to --pid-file
 */
 ("Insecure configuration for --pid-file:*"),
 ("Few location(s) are inaccessible while checking PID filepath"),
 /*
   Following WL#12670, this warning is expected.
 */
 ("Setting named_pipe_full_access_group='\\*everyone\\*' is insecure"),

 /* ASan memory allocation warnings */
 ("==[0-9]*== WARNING: AddressSanitizer failed to allocate 0x[0-9a-f]+ bytes"),

 /*
   Galera suppressions
 */
 ("WSREP:*down context*"),
 ("WSREP: Failed to send state UUID:*"),
 ("WSREP: wsrep_sst_receive_address is set to '127.0.0.1"),
 ("WSREP: option --wsrep-causal-reads is deprecated"),
 ("WSREP: --wsrep-causal-reads=ON takes precedence over --wsrep-sync-wait=0"),
 ("WSREP: Could not open saved state file for reading: "),
 ("WSREP: Could not open state file for reading: "),
 ("WSREP: access file\\(.*gvwstate\\.dat\\) failed\\(No such file or directory\\)"),
 ("WSREP: Gap in state sequence\\. Need state transfer\\."),
 ("WSREP: Failed to prepare for incremental state transfer: Local state UUID \\(00000000-0000-0000-0000-000000000000\\) does not match group state UUID"),
 ("WSREP: No existing UUID has been found, so we assume that this is the first time that this server has been started\\. Generating a new UUID: "),
 ("WSREP: last inactive check more than"),
 ("WSREP: binlog cache not empty \\(0 bytes\\) at connection close"),
 ("WSREP: SQL statement was ineffective"),
 ("WSREP: Refusing exit for the last slave thread"),
 ("WSREP: Quorum: No node with complete state"),
 ("WSREP: Failed to report last committed"),
 ("Slave SQL: Error 'Duplicate entry"),
 ("Query apply warning:"),
 ("WSREP: Ignoring error for TO isolated action:"),
 ("WSREP: Initial position was provided by configuration or SST, avoiding override"),
 ("Warning: Using a password on the command line interface can be insecure"),
 ("InnoDB: Error: Table \"mysql\"\\.\"innodb_table_stats\" not found"),
 ("but it is impossible to select State Transfer donor: Resource temporarily unavailable"),
 ("WSREP: Could not find peer"),
 ("WSREP: discarding established \\(time wait\\)"),
 ("sending install message failed: Resource temporarily unavailable"),
 ("WSREP: Ignoring possible split-brain \\(allowed by configuration\\) from view"),
 ("WSREP: no nodes coming from prim view, prim not possible"),
 ("WSREP: Failed to prepare for incremental state transfer: Local state seqno is undefined:"),
 ("WSREP: gcs_caused\\(\\) returned -107 \\(Transport endpoint is not connected\\)"),
 ("WSREP: gcs_caused\\(\\) returned -57 \\(Socket is not connected\\)"),
 ("WSREP: gcs_caused\\(\\) returned -1 \\(Operation not permitted\\)"),
 ("Action message in non-primary configuration from member"),
 ("SYNC message from member"),
 ("InnoDB: Resizing redo log from"),
 ("InnoDB: Starting to delete and rewrite log files"),
 ("InnoDB: New log files created, LSN="),
-- WSREP: Send action {0x7f86280147f0, 73, STATE_REQUEST} returned -107 (Transport endpoint is not connected)
 ("Transport endpoint is not connected"),
 ("Socket is not connected"),
-- "WSREP: Protocol violation. JOIN message sender 1.0 (host-91-221-67-96) is not in state transfer (SYNCED). Message ignored.
 ("is not in state transfer"),
 ("JOIN message from member .* in non-primary configuration"),
 ("install timer expired"),
 ("Last Applied Action message in non-primary configuration from member"),
 ("IP address '127.0.0.2' could not be resolved: Name or service not known"),

 ("THE_LAST_SUPPRESSION")||


--
-- Procedure that uses the above created tables to check
-- the servers error log for warnings
--
CREATE DEFINER=root@localhost PROCEDURE check_warnings(OUT result INT)
BEGIN
  DECLARE `pos` bigint unsigned;

  -- Don't write these queries to binlog
  SET SQL_LOG_BIN=0;

  --
  -- Remove mark from lines that are suppressed by global suppressions
  --
  UPDATE error_log el, global_suppressions gs
    SET suspicious=0
      WHERE el.suspicious=1 AND el.line REGEXP gs.pattern;

  --
  -- Remove mark from lines that are suppressed by test specific suppressions
  --
  UPDATE error_log el, test_suppressions ts
    SET suspicious=0
      WHERE el.suspicious=1 AND el.line REGEXP ts.pattern;

  --
  -- Get the number of marked lines and return result
  --
  SELECT COUNT(*) INTO @num_warnings FROM error_log
    WHERE suspicious=1;

  IF @num_warnings > 0 THEN
    SELECT line
        FROM error_log WHERE suspicious=1;
    --SELECT * FROM test_suppressions;
    -- Return 2 -> check failed
    SELECT 2 INTO result;
  ELSE
    -- Return 0 -> OK
    SELECT 0 INTO RESULT;
  END IF;

  -- Cleanup for next test
  IF @@wsrep_on = 1 THEN
    -- The TRUNCATE should not be replicated under Galera
    -- as it causes the custom suppressions on the other
    -- nodes to be deleted as well
    SET wsrep_on = 0;
  TRUNCATE test_suppressions;
    SET wsrep_on = 1;
  ELSE 
    TRUNCATE test_suppressions;
  END IF;    

  DROP TABLE error_log;

END||

--
-- Declare a procedure testcases can use to insert test
-- specific suppressions
--
/*!50001
CREATE DEFINER=root@localhost
PROCEDURE add_suppression(pattern VARCHAR(255))
BEGIN
  INSERT INTO test_suppressions (pattern) VALUES (pattern);
END
*/||


