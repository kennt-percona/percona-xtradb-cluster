.. _binlogging_replication_improvements:

=======================================
Binlogging and replication improvements
=======================================

Due to continuous development, |Percona Server| incorporated a number of
improvements related to replication and binary logs handling. This resulted in
replication specifics, which distinguishes it from |MySQL|.

Temporary tables and mixed logging format
=========================================

Summary of the fix:
*******************

As soon as some statement involving temporary table was met when using mixed
binlog format, |MySQL| was switching to row-based logging of all statements the
end of the session or until all temporary tables used in this session are
dropped. It is inconvenient in case of long lasting connections, including
replication-related ones. |Percona Server| fixes the situation by switching
between statement-based and row-based logging as and when necessary.

Version Specific Information
****************************

  * :rn:`5.6.21-70.1`
    Fix implemented in |Percona Server| 5.6

Details:
********

Mixed binary logging format supported by |Percona Server| means that
server runs in statement-based logging by default, but switches to row-based
logging when replication would be unpredictable - in the case of a
nondeterministic SQL statement that may cause data divergence if reproduced on
a slave server. The switch is done upon any condition from the long list, and
one of these conditions is the use of temporary tables.

Temporary tables are **never** logged using row-based format, but any
statement, that touches a temporary table, is logged in row mode. This way all
the side effects that temporary tables may produce on non-temporary ones are
intercepted.

There is no need to use row logging format for any other statements solely
because of the temp table presence. However |MySQL| was undertaking such an
excessive precaution: once some statement with temporary table had appeared and
the row-based logging was used, |MySQL| logged unconditionally all
subsequent statements in row format.

Percona Server have implemented more accurate behavior: instead of switching to
row-based logging until the last temporary table is closed, the usual rules of
row vs statement format apply, and presence of currently opened temporary
tables is no longer considered. This change was introduced with the fix of a
bug :psbug:`151` (upstream :mysqlbug:`72475`).

Temporary table drops and binloging on GTID-enabled server
==========================================================

Summary of the fix:
*******************

MySQL logs DROP statements for all temporary tables irrelative of the logging
mode under which these tables were created. This produces binlog writes and
errand GTIDs on slaves with row and mixed logging. |Percona Server| fixes this
by tracking the binlog format at temporary table create time and using it to
decide whether a DROP should be logged or not.

Version Specific Information
****************************

  * :rn:`5.6.38-83.0`
    Fix implemented in |Percona Server| 5.6

Details:
********

Even with read_only mode enabled, the server permits some operations, including
ones with temporary tables. With the previous fix, temporary table operations
are not binlogged in row or mixed mode. But |MySQL| doesn’t track what was
the logging mode when temporary table was created, and therefore
unconditionally logs ``DROP`` statements for all temporary tables. These
``DROP`` statements receive ``IF EXISTS`` addition, which is intended to make
them harmless.

|Percona Server| have fixed this with the bug fixes :psbug:`964`, upstream
:mysqlbug:`83003`, and upstream :mysqlbug:`85258`. Moreover, after all the
binlogging fixes discussed so far nothing involving temporary tables is logged
to binary log in row or mixed format, and so there is no need to consider
``CREATE/DROP TEMPORARY TABLE`` unsafe for use in stored functions, triggers,
and multi-statement transactions in row/mixed format. Therefore an additional
fix was introduced to mark creation and drop of temporary tables as unsafe
inside transactions in statement-based replication only (bug fixed
:psbug:`1816`, upstream :mysqlbug:`89467`)).

Safety of statements with a ``LIMIT`` clause
============================================

Summary of the fix:
*******************

|MySQL| considers all ``UPDATE/DELETE/INSERT ... SELECT`` statements with
``LIMIT`` clause to be unsafe, no matter wether they are really producing
non-deterministic result or not, and switches from statement-based logging
to row-based one. |Percona Server| is more accurate, it acknowledges such
instructions as safe when they include ``ORDER BY PK`` or ``WHERE``
condition. This fix has been ported from the upstream bug report
:mysqlbug:`42415` (:psbug:`44`).

Version Specific Information
****************************

  * :rn:`5.6.13-60.5`
    Fix ported from |Percona Server| 5.5



