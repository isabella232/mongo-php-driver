/*
 * Copyright 2014-2017 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* External libs */
#include "bson/bson.h"
#include "mongoc/mongoc.h"

/* PHP Core stuff */
#include <php.h>
#include <php_ini.h>
#include <ext/standard/info.h>
#include <ext/standard/file.h>
#include <Zend/zend_hash.h>
#include <Zend/zend_interfaces.h>
#include <Zend/zend_exceptions.h>
#include <ext/spl/spl_iterators.h>
#include <ext/spl/spl_exceptions.h>
#include <ext/standard/php_var.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <Zend/zend_smart_str.h>

#ifdef MONGOC_ENABLE_CLIENT_SIDE_ENCRYPTION
#include <mongocrypt/mongocrypt.h>
#endif

/* getpid() */
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef PHP_WIN32
#include <process.h>
#endif

/* Stream wrapper */
#include <main/php_streams.h>
#include <main/php_network.h>
/* Debug log writing */
#include <main/php_open_temporary_file.h>
/* For formating timestamp in the log */
#include <ext/date/php_date.h>
/* String manipulation */
#include <Zend/zend_string.h>
/* PHP array helpers */
#include "php_array_api.h"

/* Our Compatability header */
#include "phongo_compat.h"

/* Our stuffz */
#include "php_phongo.h"
#include "php_bson.h"
#include "src/BSON/functions.h"
#include "src/MongoDB/Monitoring/functions.h"

#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "PHONGO"

#define PHONGO_DEBUG_INI "mongodb.debug"
#define PHONGO_DEBUG_INI_DEFAULT ""
#define PHONGO_METADATA_SEPARATOR " / "
#define PHONGO_METADATA_SEPARATOR_LEN (sizeof(PHONGO_METADATA_SEPARATOR) - 1)

ZEND_DECLARE_MODULE_GLOBALS(mongodb)
#if defined(ZTS) && defined(COMPILE_DL_MONGODB)
ZEND_TSRMLS_CACHE_DEFINE();
#endif

/* Initialize a thread counter, which will be atomically incremented in GINIT.
 * In turn, GSHUTDOWN will decrement the counter and call mongoc_cleanup() when
 * it reaches zero (i.e. last thread is shutdown). This is necessary because
 * mongoc_cleanup() must be called after all persistent clients have been
 * destroyed. */
static int32_t phongo_num_threads = 0;

/* Declare zend_class_entry dependencies, which are initialized in MINIT */
zend_class_entry* php_phongo_date_immutable_ce;
zend_class_entry* php_phongo_json_serializable_ce;

php_phongo_server_description_type_map_t
	php_phongo_server_description_type_map[PHONGO_SERVER_DESCRIPTION_TYPES] = {
		{ PHONGO_SERVER_UNKNOWN, "Unknown" },
		{ PHONGO_SERVER_STANDALONE, "Standalone" },
		{ PHONGO_SERVER_MONGOS, "Mongos" },
		{ PHONGO_SERVER_POSSIBLE_PRIMARY, "PossiblePrimary" },
		{ PHONGO_SERVER_RS_PRIMARY, "RSPrimary" },
		{ PHONGO_SERVER_RS_SECONDARY, "RSSecondary" },
		{ PHONGO_SERVER_RS_ARBITER, "RSArbiter" },
		{ PHONGO_SERVER_RS_OTHER, "RSOther" },
		{ PHONGO_SERVER_RS_GHOST, "RSGhost" },
	};

/* {{{ phongo_std_object_handlers */
zend_object_handlers phongo_std_object_handlers;

zend_object_handlers* phongo_get_std_object_handlers(void)
{
	return &phongo_std_object_handlers;
}
/* }}} */

/* Forward declarations */
static bool phongo_split_namespace(const char* namespace, char** dbname, char** cname);

/* {{{ Error reporting and logging */
zend_class_entry* phongo_exception_from_phongo_domain(php_phongo_error_domain_t domain)
{
	switch (domain) {
		case PHONGO_ERROR_INVALID_ARGUMENT:
			return php_phongo_invalidargumentexception_ce;
		case PHONGO_ERROR_LOGIC:
			return php_phongo_logicexception_ce;
		case PHONGO_ERROR_RUNTIME:
			return php_phongo_runtimeexception_ce;
		case PHONGO_ERROR_UNEXPECTED_VALUE:
			return php_phongo_unexpectedvalueexception_ce;
		case PHONGO_ERROR_MONGOC_FAILED:
			return php_phongo_runtimeexception_ce;
		case PHONGO_ERROR_CONNECTION_FAILED:
			return php_phongo_connectionexception_ce;
	}

	MONGOC_ERROR("Resolving unknown phongo error domain: %d", domain);
	return php_phongo_runtimeexception_ce;
}
zend_class_entry* phongo_exception_from_mongoc_domain(mongoc_error_domain_t domain, mongoc_error_code_t code)
{
	if (domain == MONGOC_ERROR_CLIENT) {
		if (code == MONGOC_ERROR_CLIENT_AUTHENTICATE) {
			return php_phongo_authenticationexception_ce;
		}

		if (code == MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG) {
			return php_phongo_invalidargumentexception_ce;
		}
	}

	if (domain == MONGOC_ERROR_COMMAND && code == MONGOC_ERROR_COMMAND_INVALID_ARG) {
		return php_phongo_invalidargumentexception_ce;
	}

	if (domain == MONGOC_ERROR_SERVER) {
		if (code == PHONGO_SERVER_ERROR_EXCEEDED_TIME_LIMIT) {
			return php_phongo_executiontimeoutexception_ce;
		}

		return php_phongo_serverexception_ce;
	}

	if (domain == MONGOC_ERROR_SERVER_SELECTION && code == MONGOC_ERROR_SERVER_SELECTION_FAILURE) {
		return php_phongo_connectiontimeoutexception_ce;
	}

	if (domain == MONGOC_ERROR_STREAM) {
		if (code == MONGOC_ERROR_STREAM_SOCKET) {
			return php_phongo_connectiontimeoutexception_ce;
		}

		return php_phongo_connectionexception_ce;
	}

	if (domain == MONGOC_ERROR_WRITE_CONCERN) {
		return php_phongo_serverexception_ce;
	}

	if (domain == MONGOC_ERROR_PROTOCOL && code == MONGOC_ERROR_PROTOCOL_BAD_WIRE_VERSION) {
		return php_phongo_connectionexception_ce;
	}

	if (domain == MONGOC_ERROR_CLIENT_SIDE_ENCRYPTION) {
		return php_phongo_encryptionexception_ce;
	}

	return php_phongo_runtimeexception_ce;
}
void phongo_throw_exception(php_phongo_error_domain_t domain, const char* format, ...)
{
	va_list args;
	char*   message;
	int     message_len;

	va_start(args, format);
	message_len = vspprintf(&message, 0, format, args);
	zend_throw_exception(phongo_exception_from_phongo_domain(domain), message, 0);
	efree(message);
	va_end(args);
}

static int phongo_exception_append_error_labels(zval* labels, const bson_iter_t* iter)
{
	bson_iter_t error_labels;
	uint32_t    label_count = 0;

	if (!BSON_ITER_HOLDS_ARRAY(iter) || !bson_iter_recurse(iter, &error_labels)) {
		return label_count;
	}

	while (bson_iter_next(&error_labels)) {
		if (BSON_ITER_HOLDS_UTF8(&error_labels)) {
			const char* error_label;
			uint32_t    error_label_len;

			error_label = bson_iter_utf8(&error_labels, &error_label_len);
			ADD_NEXT_INDEX_STRINGL(labels, error_label, error_label_len);
			label_count++;
		}
	}

	return label_count;
}

static void phongo_exception_add_error_labels(const bson_t* reply)
{
	bson_iter_t iter, child;
	zval        labels;
	uint32_t    label_count = 0;

	if (!reply) {
		return;
	}

	array_init(&labels);

	if (bson_iter_init_find(&iter, reply, "errorLabels")) {
		label_count += phongo_exception_append_error_labels(&labels, &iter);
	}

	if (bson_iter_init_find(&iter, reply, "writeConcernError") && BSON_ITER_HOLDS_DOCUMENT(&iter) &&
		bson_iter_recurse(&iter, &child) && bson_iter_find(&child, "errorLabels")) {
		label_count += phongo_exception_append_error_labels(&labels, &child);
	}

	/* mongoc_write_result_t always reports writeConcernErrors in an array, so
	 * we must iterate this to collect WCE labels for BulkWrite replies. */
	if (bson_iter_init_find(&iter, reply, "writeConcernErrors") && BSON_ITER_HOLDS_ARRAY(&iter) && bson_iter_recurse(&iter, &child)) {
		bson_iter_t wce;

		while (bson_iter_next(&child)) {
			if (BSON_ITER_HOLDS_DOCUMENT(&child) && bson_iter_recurse(&child, &wce) && bson_iter_find(&wce, "errorLabels")) {
				label_count += phongo_exception_append_error_labels(&labels, &wce);
			}
		}
	}

	if (label_count > 0) {
		phongo_add_exception_prop(ZEND_STRL("errorLabels"), &labels);
	}

	zval_ptr_dtor(&labels);
}

void phongo_throw_exception_from_bson_error_t_and_reply(bson_error_t* error, const bson_t* reply)
{
	/* Server errors (other than ExceededTimeLimit) and write concern errors
	 * may use CommandException and report the result document for the
	 * failed command. For BC, ExceededTimeLimit errors will continue to use
	 * ExcecutionTimeoutException and omit the result document. */
	if (reply && ((error->domain == MONGOC_ERROR_SERVER && error->code != PHONGO_SERVER_ERROR_EXCEEDED_TIME_LIMIT) || error->domain == MONGOC_ERROR_WRITE_CONCERN)) {
		zval zv;

		zend_throw_exception(php_phongo_commandexception_ce, error->message, error->code);
		if (php_phongo_bson_to_zval(bson_get_data(reply), reply->len, &zv)) {
			phongo_add_exception_prop(ZEND_STRL("resultDocument"), &zv);
		}

		zval_ptr_dtor(&zv);
	} else {
		zend_throw_exception(phongo_exception_from_mongoc_domain(error->domain, error->code), error->message, error->code);
	}
	phongo_exception_add_error_labels(reply);
}

void phongo_throw_exception_from_bson_error_t(bson_error_t* error)
{
	phongo_throw_exception_from_bson_error_t_and_reply(error, NULL);
}

static void php_phongo_log(mongoc_log_level_t log_level, const char* log_domain, const char* message, void* user_data)
{
	struct timeval tv;
	time_t         t;
	zend_long      tu;
	zend_string*   dt;

	(void) user_data;

	gettimeofday(&tv, NULL);
	t  = tv.tv_sec;
	tu = tv.tv_usec;

	dt = php_format_date((char*) ZEND_STRL("Y-m-d\\TH:i:s"), t, 0);

	fprintf(MONGODB_G(debug_fd), "[%s.%06" PHONGO_LONG_FORMAT "+00:00] %10s: %-8s> %s\n", ZSTR_VAL(dt), tu, log_domain, mongoc_log_level_str(log_level), message);
	fflush(MONGODB_G(debug_fd));
	efree(dt);
}

/* }}} */

/* {{{ Init objects */
static void phongo_cursor_init(zval* return_value, zval* manager, mongoc_cursor_t* cursor, zval* readPreference, zval* session) /* {{{ */
{
	php_phongo_cursor_t* intern;

	object_init_ex(return_value, php_phongo_cursor_ce);

	intern            = Z_CURSOR_OBJ_P(return_value);
	intern->cursor    = cursor;
	intern->server_id = mongoc_cursor_get_hint(cursor);
	intern->advanced  = false;
	intern->current   = 0;

	ZVAL_ZVAL(&intern->manager, manager, 1, 0);

	if (readPreference) {
		ZVAL_ZVAL(&intern->read_preference, readPreference, 1, 0);
	}

	if (session) {
		ZVAL_ZVAL(&intern->session, session, 1, 0);
	}
} /* }}} */

static void phongo_cursor_init_for_command(zval* return_value, zval* manager, mongoc_cursor_t* cursor, const char* db, zval* command, zval* readPreference, zval* session) /* {{{ */
{
	php_phongo_cursor_t* intern;

	phongo_cursor_init(return_value, manager, cursor, readPreference, session);
	intern = Z_CURSOR_OBJ_P(return_value);

	intern->database = estrdup(db);

	ZVAL_ZVAL(&intern->command, command, 1, 0);
} /* }}} */

static void phongo_cursor_init_for_query(zval* return_value, zval* manager, mongoc_cursor_t* cursor, const char* namespace, zval* query, zval* readPreference, zval* session) /* {{{ */
{
	php_phongo_cursor_t* intern;

	phongo_cursor_init(return_value, manager, cursor, readPreference, session);
	intern = Z_CURSOR_OBJ_P(return_value);

	/* namespace has already been validated by phongo_execute_query() */
	phongo_split_namespace(namespace, &intern->database, &intern->collection);

	/* cursor has already been advanced by phongo_execute_query() calling
	 * phongo_cursor_advance_and_check_for_error() */
	intern->advanced = true;

	ZVAL_ZVAL(&intern->query, query, 1, 0);
} /* }}} */

void phongo_server_init(zval* return_value, zval* manager, uint32_t server_id) /* {{{ */
{
	php_phongo_server_t* server;

	object_init_ex(return_value, php_phongo_server_ce);

	server            = Z_SERVER_OBJ_P(return_value);
	server->server_id = server_id;

	ZVAL_ZVAL(&server->manager, manager, 1, 0);
}
/* }}} */

void phongo_session_init(zval* return_value, zval* manager, mongoc_client_session_t* client_session) /* {{{ */
{
	php_phongo_session_t* session;

	object_init_ex(return_value, php_phongo_session_ce);

	session                 = Z_SESSION_OBJ_P(return_value);
	session->client_session = client_session;

	ZVAL_ZVAL(&session->manager, manager, 1, 0);
}
/* }}} */

void phongo_readconcern_init(zval* return_value, const mongoc_read_concern_t* read_concern) /* {{{ */
{
	php_phongo_readconcern_t* intern;

	object_init_ex(return_value, php_phongo_readconcern_ce);

	intern               = Z_READCONCERN_OBJ_P(return_value);
	intern->read_concern = mongoc_read_concern_copy(read_concern);
}
/* }}} */

void phongo_readpreference_init(zval* return_value, const mongoc_read_prefs_t* read_prefs) /* {{{ */
{
	php_phongo_readpreference_t* intern;

	object_init_ex(return_value, php_phongo_readpreference_ce);

	intern                  = Z_READPREFERENCE_OBJ_P(return_value);
	intern->read_preference = mongoc_read_prefs_copy(read_prefs);
}
/* }}} */

void phongo_writeconcern_init(zval* return_value, const mongoc_write_concern_t* write_concern) /* {{{ */
{
	php_phongo_writeconcern_t* intern;

	object_init_ex(return_value, php_phongo_writeconcern_ce);

	intern                = Z_WRITECONCERN_OBJ_P(return_value);
	intern->write_concern = mongoc_write_concern_copy(write_concern);
}
/* }}} */

zend_bool phongo_writeconcernerror_init(zval* return_value, bson_t* bson) /* {{{ */
{
	bson_iter_t                     iter;
	php_phongo_writeconcernerror_t* intern;

	object_init_ex(return_value, php_phongo_writeconcernerror_ce);

	intern       = Z_WRITECONCERNERROR_OBJ_P(return_value);
	intern->code = 0;

	if (bson_iter_init_find(&iter, bson, "code") && BSON_ITER_HOLDS_INT32(&iter)) {
		intern->code = bson_iter_int32(&iter);
	}

	if (bson_iter_init_find(&iter, bson, "errmsg") && BSON_ITER_HOLDS_UTF8(&iter)) {
		uint32_t    errmsg_len;
		const char* err_msg = bson_iter_utf8(&iter, &errmsg_len);

		intern->message = estrndup(err_msg, errmsg_len);
	}

	if (bson_iter_init_find(&iter, bson, "errInfo") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
		uint32_t       len;
		const uint8_t* data = NULL;

		bson_iter_document(&iter, &len, &data);

		if (!php_phongo_bson_to_zval(data, len, &intern->info)) {
			zval_ptr_dtor(&intern->info);
			ZVAL_UNDEF(&intern->info);

			return false;
		}
	}

	return true;
} /* }}} */

zend_bool phongo_writeerror_init(zval* return_value, bson_t* bson) /* {{{ */
{
	bson_iter_t              iter;
	php_phongo_writeerror_t* intern;

	object_init_ex(return_value, php_phongo_writeerror_ce);

	intern        = Z_WRITEERROR_OBJ_P(return_value);
	intern->code  = 0;
	intern->index = 0;

	if (bson_iter_init_find(&iter, bson, "code") && BSON_ITER_HOLDS_INT32(&iter)) {
		intern->code = bson_iter_int32(&iter);
	}

	if (bson_iter_init_find(&iter, bson, "errmsg") && BSON_ITER_HOLDS_UTF8(&iter)) {
		uint32_t    errmsg_len;
		const char* err_msg = bson_iter_utf8(&iter, &errmsg_len);

		intern->message = estrndup(err_msg, errmsg_len);
	}

	if (bson_iter_init_find(&iter, bson, "errInfo") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
		uint32_t       len;
		const uint8_t* data = NULL;

		bson_iter_document(&iter, &len, &data);

		if (!php_phongo_bson_to_zval(data, len, &intern->info)) {
			zval_ptr_dtor(&intern->info);
			ZVAL_UNDEF(&intern->info);

			return false;
		}
	}

	if (bson_iter_init_find(&iter, bson, "index") && BSON_ITER_HOLDS_INT32(&iter)) {
		intern->index = bson_iter_int32(&iter);
	}

	return true;
} /* }}} */

static php_phongo_writeresult_t* phongo_writeresult_init(zval* return_value, bson_t* reply, zval* manager, uint32_t server_id) /* {{{ */
{
	php_phongo_writeresult_t* writeresult;

	object_init_ex(return_value, php_phongo_writeresult_ce);

	writeresult            = Z_WRITERESULT_OBJ_P(return_value);
	writeresult->reply     = bson_copy(reply);
	writeresult->server_id = server_id;

	ZVAL_ZVAL(&writeresult->manager, manager, 1, 0);

	return writeresult;
} /* }}} */
/* }}} */

/* {{{ CRUD */
/* Splits a namespace name into the database and collection names, allocated with estrdup. */
static bool phongo_split_namespace(const char* namespace, char** dbname, char** cname) /* {{{ */
{
	char* dot = strchr(namespace, '.');

	if (!dot) {
		return false;
	}

	if (cname) {
		*cname = estrdup(namespace + (dot - namespace) + 1);
	}
	if (dbname) {
		*dbname = estrndup(namespace, dot - namespace);
	}

	return true;
} /* }}} */

/* Parses the "readConcern" option for an execute method. If mongoc_opts is not
 * NULL, the option will be appended. On error, false is returned and an
 * exception is thrown. */
static bool phongo_parse_read_concern(zval* options, bson_t* mongoc_opts) /* {{{ */
{
	zval*                  option = NULL;
	mongoc_read_concern_t* read_concern;

	if (!options) {
		return true;
	}

	if (Z_TYPE_P(options) != IS_ARRAY) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected options to be array, %s given", PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(options));
		return false;
	}

	option = php_array_fetchc(options, "readConcern");

	if (!option) {
		return true;
	}

	if (Z_TYPE_P(option) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(option), php_phongo_readconcern_ce)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"readConcern\" option to be %s, %s given", ZSTR_VAL(php_phongo_readconcern_ce->name), PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(option));
		return false;
	}

	read_concern = Z_READCONCERN_OBJ_P(option)->read_concern;

	if (mongoc_opts && !mongoc_read_concern_append(read_concern, mongoc_opts)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Error appending \"readConcern\" option");
		return false;
	}

	return true;
} /* }}} */

/* Parses the "readPreference" option for an execute method. If zreadPreference
 * is not NULL, it will be assigned to the option. On error, false is returned
 * and an exception is thrown. */
bool phongo_parse_read_preference(zval* options, zval** zreadPreference) /* {{{ */
{
	zval* option = NULL;

	if (!options) {
		return true;
	}

	if (Z_TYPE_P(options) != IS_ARRAY) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected options to be array, %s given", PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(options));
		return false;
	}

	option = php_array_fetchc(options, "readPreference");

	if (!option) {
		return true;
	}

	if (Z_TYPE_P(option) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(option), php_phongo_readpreference_ce)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"readPreference\" option to be %s, %s given", ZSTR_VAL(php_phongo_readpreference_ce->name), PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(option));
		return false;
	}

	if (zreadPreference) {
		*zreadPreference = option;
	}

	return true;
} /* }}} */

/* Parses the "session" option for an execute method. The client object should
 * correspond to the Manager executing the operation and will be used to ensure
 * that the session is correctly associated with that client. If mongoc_opts is
 * not NULL, the option will be appended. If zsession is not NULL, it will be
 * assigned to the option. On error, false is returned and an exception is
 * thrown. */
bool phongo_parse_session(zval* options, mongoc_client_t* client, bson_t* mongoc_opts, zval** zsession) /* {{{ */
{
	zval*                          option = NULL;
	const mongoc_client_session_t* client_session;

	if (!options) {
		return true;
	}

	if (Z_TYPE_P(options) != IS_ARRAY) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected options to be array, %s given", PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(options));
		return false;
	}

	option = php_array_fetchc(options, "session");

	if (!option) {
		return true;
	}

	if (Z_TYPE_P(option) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(option), php_phongo_session_ce)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"session\" option to be %s, %s given", ZSTR_VAL(php_phongo_session_ce->name), PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(option));
		return false;
	}

	client_session = Z_SESSION_OBJ_P(option)->client_session;

	if (client != mongoc_client_session_get_client(client_session)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Cannot use Session started from a different Manager");
		return false;
	}

	if (mongoc_opts && !mongoc_client_session_append(client_session, mongoc_opts, NULL)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Error appending \"session\" option");
		return false;
	}

	if (zsession) {
		*zsession = option;
	}

	return true;
} /* }}} */

/* Parses the "writeConcern" option for an execute method. If mongoc_opts is not
 * NULL, the option will be appended. If zwriteConcern is not NULL, it will be
 * assigned to the option. On error, false is returned and an exception is
 * thrown. */
static bool phongo_parse_write_concern(zval* options, bson_t* mongoc_opts, zval** zwriteConcern) /* {{{ */
{
	zval*                   option = NULL;
	mongoc_write_concern_t* write_concern;

	if (!options) {
		return true;
	}

	if (Z_TYPE_P(options) != IS_ARRAY) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected options to be array, %s given", PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(options));
		return false;
	}

	option = php_array_fetchc(options, "writeConcern");

	if (!option) {
		return true;
	}

	if (Z_TYPE_P(option) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(option), php_phongo_writeconcern_ce)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"writeConcern\" option to be %s, %s given", ZSTR_VAL(php_phongo_writeconcern_ce->name), PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(option));
		return false;
	}

	write_concern = Z_WRITECONCERN_OBJ_P(option)->write_concern;

	if (mongoc_opts && !mongoc_write_concern_append(write_concern, mongoc_opts)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Error appending \"writeConcern\" option");
		return false;
	}

	if (zwriteConcern) {
		*zwriteConcern = option;
	}

	return true;
}

bool phongo_execute_bulk_write(zval* manager, const char* namespace, php_phongo_bulkwrite_t* bulk_write, zval* options, uint32_t server_id, zval* return_value) /* {{{ */
{
	mongoc_client_t*              client = NULL;
	bson_error_t                  error  = { 0 };
	int                           success;
	bson_t                        reply = BSON_INITIALIZER;
	mongoc_bulk_operation_t*      bulk  = bulk_write->bulk;
	php_phongo_writeresult_t*     writeresult;
	zval*                         zwriteConcern = NULL;
	zval*                         zsession      = NULL;
	const mongoc_write_concern_t* write_concern = NULL;

	client = Z_MANAGER_OBJ_P(manager)->client;

	if (bulk_write->executed) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "BulkWrite objects may only be executed once and this instance has already been executed");
		return false;
	}

	if (!phongo_split_namespace(namespace, &bulk_write->database, &bulk_write->collection)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "%s: %s", "Invalid namespace provided", namespace);
		return false;
	}

	if (!phongo_parse_session(options, client, NULL, &zsession)) {
		/* Exception should already have been thrown */
		return false;
	}

	if (!phongo_parse_write_concern(options, NULL, &zwriteConcern)) {
		/* Exception should already have been thrown */
		return false;
	}

	/* If a write concern was not specified, libmongoc will use the client's
	 * write concern; however, we should still fetch it for the write result.
	 * Additionally, we need to check if an unacknowledged write concern would
	 * conflict with an explicit session. */
	write_concern = zwriteConcern ? Z_WRITECONCERN_OBJ_P(zwriteConcern)->write_concern : mongoc_client_get_write_concern(client);

	if (zsession && !mongoc_write_concern_is_acknowledged(write_concern)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Cannot combine \"session\" option with an unacknowledged write concern");
		return false;
	}

	mongoc_bulk_operation_set_database(bulk, bulk_write->database);
	mongoc_bulk_operation_set_collection(bulk, bulk_write->collection);
	mongoc_bulk_operation_set_client(bulk, client);
	mongoc_bulk_operation_set_hint(bulk, server_id);

	if (zsession) {
		ZVAL_ZVAL(&bulk_write->session, zsession, 1, 0);
		mongoc_bulk_operation_set_client_session(bulk, Z_SESSION_OBJ_P(zsession)->client_session);
	}

	if (zwriteConcern) {
		mongoc_bulk_operation_set_write_concern(bulk, Z_WRITECONCERN_OBJ_P(zwriteConcern)->write_concern);
	}

	success              = mongoc_bulk_operation_execute(bulk, &reply, &error);
	bulk_write->executed = true;

	writeresult                = phongo_writeresult_init(return_value, &reply, manager, mongoc_bulk_operation_get_hint(bulk));
	writeresult->write_concern = mongoc_write_concern_copy(write_concern);

	/* A BulkWriteException is always thrown if mongoc_bulk_operation_execute()
	 * fails to ensure that the write result is accessible. If the error does
	 * not originate from the server (e.g. socket error), throw the appropriate
	 * exception first. It will be included in BulkWriteException's message and
	 * will also be accessible via Exception::getPrevious(). */
	if (!success) {
		if (error.domain != MONGOC_ERROR_SERVER && error.domain != MONGOC_ERROR_WRITE_CONCERN) {
			phongo_throw_exception_from_bson_error_t_and_reply(&error, &reply);
		}

		/* Argument errors occur before command execution, so there is no need
		 * to layer this InvalidArgumentException behind a BulkWriteException.
		 * In practice, this will be a "Cannot do an empty bulk write" error. */
		if (error.domain == MONGOC_ERROR_COMMAND && error.code == MONGOC_ERROR_COMMAND_INVALID_ARG) {
			goto cleanup;
		}

		if (EG(exception)) {
			char* message;

			(void) spprintf(&message, 0, "Bulk write failed due to previous %s: %s", PHONGO_ZVAL_EXCEPTION_NAME(EG(exception)), error.message);
			zend_throw_exception(php_phongo_bulkwriteexception_ce, message, 0);
			efree(message);
		} else {
			zend_throw_exception(php_phongo_bulkwriteexception_ce, error.message, error.code);
		}

		/* Ensure error labels are added to the final BulkWriteException. If a
		 * previous exception was also thrown, error labels will already have
		 * been added by phongo_throw_exception_from_bson_error_t_and_reply. */
		phongo_exception_add_error_labels(&reply);
		phongo_add_exception_prop(ZEND_STRL("writeResult"), return_value);
	}

cleanup:
	bson_destroy(&reply);

	return success;
} /* }}} */

/* Advance the cursor and return whether there is an error. On error, false is
 * returned and an exception is thrown. */
bool phongo_cursor_advance_and_check_for_error(mongoc_cursor_t* cursor) /* {{{ */
{
	const bson_t* doc = NULL;

	if (!mongoc_cursor_next(cursor, &doc)) {
		bson_error_t error = { 0 };

		/* Check for connection related exceptions */
		if (EG(exception)) {
			return false;
		}

		/* Could simply be no docs, which is not an error */
		if (mongoc_cursor_error_document(cursor, &error, &doc)) {
			phongo_throw_exception_from_bson_error_t_and_reply(&error, doc);
			return false;
		}
	}

	return true;
} /* }}} */

bool phongo_execute_query(zval* manager, const char* namespace, zval* zquery, zval* options, uint32_t server_id, zval* return_value) /* {{{ */
{
	mongoc_client_t*          client;
	const php_phongo_query_t* query;
	bson_t                    opts = BSON_INITIALIZER;
	mongoc_cursor_t*          cursor;
	char*                     dbname;
	char*                     collname;
	mongoc_collection_t*      collection;
	zval*                     zreadPreference = NULL;
	zval*                     zsession        = NULL;

	client = Z_MANAGER_OBJ_P(manager)->client;

	if (!phongo_split_namespace(namespace, &dbname, &collname)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "%s: %s", "Invalid namespace provided", namespace);
		return false;
	}
	collection = mongoc_client_get_collection(client, dbname, collname);
	efree(dbname);
	efree(collname);

	query = Z_QUERY_OBJ_P(zquery);

	bson_copy_to(query->opts, &opts);

	if (query->read_concern) {
		mongoc_collection_set_read_concern(collection, query->read_concern);
	}

	if (!phongo_parse_read_preference(options, &zreadPreference)) {
		/* Exception should already have been thrown */
		mongoc_collection_destroy(collection);
		bson_destroy(&opts);
		return false;
	}

	if (!phongo_parse_session(options, client, &opts, &zsession)) {
		/* Exception should already have been thrown */
		mongoc_collection_destroy(collection);
		bson_destroy(&opts);
		return false;
	}

	if (!BSON_APPEND_INT32(&opts, "serverId", server_id)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Error appending \"serverId\" option");
		mongoc_collection_destroy(collection);
		bson_destroy(&opts);
		return false;
	}

	cursor = mongoc_collection_find_with_opts(collection, query->filter, &opts, phongo_read_preference_from_zval(zreadPreference));
	mongoc_collection_destroy(collection);
	bson_destroy(&opts);

	/* maxAwaitTimeMS must be set before the cursor is sent */
	if (query->max_await_time_ms) {
		mongoc_cursor_set_max_await_time_ms(cursor, query->max_await_time_ms);
	}

	if (!phongo_cursor_advance_and_check_for_error(cursor)) {
		mongoc_cursor_destroy(cursor);
		return false;
	}

	phongo_cursor_init_for_query(return_value, manager, cursor, namespace, zquery, zreadPreference, zsession);

	return true;
} /* }}} */

static bson_t* create_wrapped_command_envelope(const char* db, bson_t* reply)
{
	bson_t* tmp;
	size_t  max_ns_len = strlen(db) + 5 + 1; /* db + ".$cmd" + '\0' */
	char*   ns         = emalloc(max_ns_len);

	snprintf(ns, max_ns_len, "%s.$cmd", db);
	tmp = BCON_NEW("cursor", "{", "id", BCON_INT64(0), "ns", BCON_UTF8(ns), "firstBatch", "[", BCON_DOCUMENT(reply), "]", "}");
	efree(ns);

	return tmp;
}

static zval* phongo_create_implicit_session(zval* manager) /* {{{ */
{
	mongoc_client_session_t* cs;
	zval*                    zsession;

	cs = mongoc_client_start_session(Z_MANAGER_OBJ_P(manager)->client, NULL, NULL);

	if (!cs) {
		return NULL;
	}

	zsession = ecalloc(sizeof(zval), 1);

	phongo_session_init(zsession, manager, cs);

	return zsession;
} /* }}} */

bool phongo_execute_command(zval* manager, php_phongo_command_type_t type, const char* db, zval* zcommand, zval* options, uint32_t server_id, zval* return_value) /* {{{ */
{
	mongoc_client_t*            client;
	const php_phongo_command_t* command;
	bson_iter_t                 iter;
	bson_t                      reply;
	bson_error_t                error = { 0 };
	bson_t                      opts  = BSON_INITIALIZER;
	mongoc_cursor_t*            cmd_cursor;
	zval*                       zreadPreference                 = NULL;
	zval*                       zsession                        = NULL;
	bool                        result                          = false;
	bool                        free_reply                      = false;
	bool                        free_zsession                   = false;
	bool                        is_unacknowledged_write_concern = false;

	client  = Z_MANAGER_OBJ_P(manager)->client;
	command = Z_COMMAND_OBJ_P(zcommand);

	if ((type & PHONGO_OPTION_READ_CONCERN) && !phongo_parse_read_concern(options, &opts)) {
		/* Exception should already have been thrown */
		goto cleanup;
	}

	if ((type & PHONGO_OPTION_READ_PREFERENCE) && !phongo_parse_read_preference(options, &zreadPreference)) {
		/* Exception should already have been thrown */
		goto cleanup;
	}

	if (!phongo_parse_session(options, client, &opts, &zsession)) {
		/* Exception should already have been thrown */
		goto cleanup;
	}

	if (type & PHONGO_OPTION_WRITE_CONCERN) {
		zval* zwriteConcern = NULL;

		if (!phongo_parse_write_concern(options, &opts, &zwriteConcern)) {
			/* Exception should already have been thrown */
			goto cleanup;
		}

		/* Determine if the explicit or inherited write concern is
		 * unacknowledged so that we can ensure it does not conflict with an
		 * explicit or implicit session. */
		if (zwriteConcern) {
			is_unacknowledged_write_concern = !mongoc_write_concern_is_acknowledged(Z_WRITECONCERN_OBJ_P(zwriteConcern)->write_concern);
		} else if (type != PHONGO_COMMAND_RAW) {
			is_unacknowledged_write_concern = !mongoc_write_concern_is_acknowledged(mongoc_client_get_write_concern(client));
		}
	}

	if (zsession && is_unacknowledged_write_concern) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Cannot combine \"session\" option with an unacknowledged write concern");
		goto cleanup;
	}

	/* If an explicit session was not provided and the effective write concern
	 * is not unacknowledged, attempt to create an implicit client session
	 * (ignoring any errors). */
	if (!zsession && !is_unacknowledged_write_concern) {
		zsession = phongo_create_implicit_session(manager);

		if (zsession) {
			free_zsession = true;

			if (!mongoc_client_session_append(Z_SESSION_OBJ_P(zsession)->client_session, &opts, NULL)) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Error appending implicit \"sessionId\" option");
				goto cleanup;
			}
		}
	}

	if (!BSON_APPEND_INT32(&opts, "serverId", server_id)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Error appending \"serverId\" option");
		goto cleanup;
	}

	/* Although "opts" already always includes the serverId option, the read
	 * preference is added to the command parts, which is relevant for mongos
	 * command construction. */
	switch (type) {
		case PHONGO_COMMAND_RAW:
			result = mongoc_client_command_with_opts(client, db, command->bson, phongo_read_preference_from_zval(zreadPreference), &opts, &reply, &error);
			break;
		case PHONGO_COMMAND_READ:
			result = mongoc_client_read_command_with_opts(client, db, command->bson, phongo_read_preference_from_zval(zreadPreference), &opts, &reply, &error);
			break;
		case PHONGO_COMMAND_WRITE:
			result = mongoc_client_write_command_with_opts(client, db, command->bson, &opts, &reply, &error);
			break;
		case PHONGO_COMMAND_READ_WRITE:
			/* We can pass NULL as readPreference, as this argument was added historically, but has no function */
			result = mongoc_client_read_write_command_with_opts(client, db, command->bson, NULL, &opts, &reply, &error);
			break;
		default:
			/* Should never happen, but if it does: exception */
			phongo_throw_exception(PHONGO_ERROR_LOGIC, "Type '%d' should never have been passed to phongo_execute_command, please file a bug report", type);
			goto cleanup;
	}

	free_reply = true;

	if (!result) {
		phongo_throw_exception_from_bson_error_t_and_reply(&error, &reply);
		goto cleanup;
	}

	/* According to mongoc_cursor_new_from_command_reply_with_opts(), the reply
	 * bson_t is ultimately destroyed on both success and failure. */
	if (bson_iter_init_find(&iter, &reply, "cursor") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
		bson_t       initial_reply = BSON_INITIALIZER;
		bson_t       cursor_opts   = BSON_INITIALIZER;
		bson_error_t error         = { 0 };

		bson_copy_to(&reply, &initial_reply);

		bson_append_int32(&cursor_opts, "serverId", -1, server_id);

		if (command->max_await_time_ms) {
			bson_append_bool(&cursor_opts, "awaitData", -1, 1);
			bson_append_int64(&cursor_opts, "maxAwaitTimeMS", -1, command->max_await_time_ms);
			bson_append_bool(&cursor_opts, "tailable", -1, 1);
		}

		if (command->batch_size) {
			bson_append_int64(&cursor_opts, "batchSize", -1, command->batch_size);
		}

		if (zsession && !mongoc_client_session_append(Z_SESSION_OBJ_P(zsession)->client_session, &cursor_opts, &error)) {
			phongo_throw_exception_from_bson_error_t(&error);
			bson_destroy(&initial_reply);
			bson_destroy(&cursor_opts);
			result = false;
			goto cleanup;
		}

		cmd_cursor = mongoc_cursor_new_from_command_reply_with_opts(client, &initial_reply, &cursor_opts);
		bson_destroy(&cursor_opts);
	} else {
		bson_t  cursor_opts   = BSON_INITIALIZER;
		bson_t* wrapped_reply = create_wrapped_command_envelope(db, &reply);

		bson_append_int32(&cursor_opts, "serverId", -1, server_id);
		cmd_cursor = mongoc_cursor_new_from_command_reply_with_opts(client, wrapped_reply, &cursor_opts);
		bson_destroy(&cursor_opts);
	}

	phongo_cursor_init_for_command(return_value, manager, cmd_cursor, db, zcommand, zreadPreference, zsession);

cleanup:
	bson_destroy(&opts);

	if (free_reply) {
		bson_destroy(&reply);
	}

	if (free_zsession) {
		zval_ptr_dtor(zsession);
		efree(zsession);
	}

	return result;
} /* }}} */
/* }}} */

/* {{{ mongoc types from from_zval */
const mongoc_write_concern_t* phongo_write_concern_from_zval(zval* zwrite_concern) /* {{{ */
{
	if (zwrite_concern) {
		php_phongo_writeconcern_t* intern = Z_WRITECONCERN_OBJ_P(zwrite_concern);

		if (intern) {
			return intern->write_concern;
		}
	}

	return NULL;
} /* }}} */

const mongoc_read_concern_t* phongo_read_concern_from_zval(zval* zread_concern) /* {{{ */
{
	if (zread_concern) {
		php_phongo_readconcern_t* intern = Z_READCONCERN_OBJ_P(zread_concern);

		if (intern) {
			return intern->read_concern;
		}
	}

	return NULL;
} /* }}} */

const mongoc_read_prefs_t* phongo_read_preference_from_zval(zval* zread_preference) /* {{{ */
{
	if (zread_preference) {
		php_phongo_readpreference_t* intern = Z_READPREFERENCE_OBJ_P(zread_preference);

		if (intern) {
			return intern->read_preference;
		}
	}

	return NULL;
} /* }}} */
/* }}} */

/* {{{ phongo zval from mongoc types */
php_phongo_server_description_type_t php_phongo_server_description_type(mongoc_server_description_t* sd)
{
	const char* name = mongoc_server_description_type(sd);
	int         i;

	for (i = 0; i < PHONGO_SERVER_DESCRIPTION_TYPES; i++) {
		if (!strcmp(name, php_phongo_server_description_type_map[i].name)) {
			return php_phongo_server_description_type_map[i].type;
		}
	}

	return PHONGO_SERVER_UNKNOWN;
}

bool php_phongo_server_to_zval(zval* retval, mongoc_server_description_t* sd) /* {{{ */
{
	mongoc_host_list_t* host      = mongoc_server_description_host(sd);
	const bson_t*       is_master = mongoc_server_description_ismaster(sd);
	bson_iter_t         iter;

	array_init(retval);

	ADD_ASSOC_STRING(retval, "host", host->host);
	ADD_ASSOC_LONG_EX(retval, "port", host->port);
	ADD_ASSOC_LONG_EX(retval, "type", php_phongo_server_description_type(sd));
	ADD_ASSOC_BOOL_EX(retval, "is_primary", !strcmp(mongoc_server_description_type(sd), php_phongo_server_description_type_map[PHONGO_SERVER_RS_PRIMARY].name));
	ADD_ASSOC_BOOL_EX(retval, "is_secondary", !strcmp(mongoc_server_description_type(sd), php_phongo_server_description_type_map[PHONGO_SERVER_RS_SECONDARY].name));
	ADD_ASSOC_BOOL_EX(retval, "is_arbiter", !strcmp(mongoc_server_description_type(sd), php_phongo_server_description_type_map[PHONGO_SERVER_RS_ARBITER].name));
	ADD_ASSOC_BOOL_EX(retval, "is_hidden", bson_iter_init_find_case(&iter, is_master, "hidden") && bson_iter_as_bool(&iter));
	ADD_ASSOC_BOOL_EX(retval, "is_passive", bson_iter_init_find_case(&iter, is_master, "passive") && bson_iter_as_bool(&iter));

	if (bson_iter_init_find(&iter, is_master, "tags") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
		const uint8_t*        bytes;
		uint32_t              len;
		php_phongo_bson_state state;

		PHONGO_BSON_INIT_DEBUG_STATE(state);
		bson_iter_document(&iter, &len, &bytes);
		if (!php_phongo_bson_to_zval_ex(bytes, len, &state)) {
			zval_ptr_dtor(&state.zchild);
			return false;
		}

		ADD_ASSOC_ZVAL_EX(retval, "tags", &state.zchild);
	}

	{
		php_phongo_bson_state state;

		PHONGO_BSON_INIT_DEBUG_STATE(state);

		if (!php_phongo_bson_to_zval_ex(bson_get_data(is_master), is_master->len, &state)) {
			zval_ptr_dtor(&state.zchild);
			return false;
		}

		ADD_ASSOC_ZVAL_EX(retval, "last_is_master", &state.zchild);
	}
	ADD_ASSOC_LONG_EX(retval, "round_trip_time", (zend_long) mongoc_server_description_round_trip_time(sd));

	return true;
} /* }}} */

void php_phongo_read_concern_to_zval(zval* retval, const mongoc_read_concern_t* read_concern) /* {{{ */
{
	const char* level = mongoc_read_concern_get_level(read_concern);

	array_init_size(retval, 1);

	if (level) {
		ADD_ASSOC_STRING(retval, "level", level);
	}
} /* }}} */

/* If options is not an array, insert it as a field in a newly allocated array.
 * This may be used to convert legacy options (e.g. ReadPreference option for
 * an executeQuery method) into an options array.
 *
 * A pointer to the array zval will always be returned. If allocated is set to
 * true, php_phongo_prep_legacy_option_free() should be used to free the array
 * zval later. */
zval* php_phongo_prep_legacy_option(zval* options, const char* key, bool* allocated) /* {{{ */
{
	*allocated = false;

	if (options && Z_TYPE_P(options) != IS_ARRAY) {
		zval* new_options = ecalloc(sizeof(zval), 1);

		array_init_size(new_options, 1);
		add_assoc_zval(new_options, key, options);
		Z_ADDREF_P(options);
		*allocated = true;

		return new_options;
	}

	return options;
} /* }}} */

void php_phongo_prep_legacy_option_free(zval* options) /* {{{ */
{
	zval_ptr_dtor(options);
	efree(options);
} /* }}} */

/* Prepare tagSets for BSON encoding by converting each array in the set to an
 * object. This ensures that empty arrays will serialize as empty documents.
 *
 * php_phongo_read_preference_tags_are_valid() handles actual validation of the
 * tag set structure. */
void php_phongo_read_preference_prep_tagsets(zval* tagSets) /* {{{ */
{
	HashTable* ht_data;
	zval*      tagSet;

	if (Z_TYPE_P(tagSets) != IS_ARRAY) {
		return;
	}

	ht_data = HASH_OF(tagSets);

	ZEND_HASH_FOREACH_VAL_IND(ht_data, tagSet)
	{
		ZVAL_DEREF(tagSet);
		if (Z_TYPE_P(tagSet) == IS_ARRAY) {
			SEPARATE_ZVAL_NOREF(tagSet);
			convert_to_object(tagSet);
		}
	}
	ZEND_HASH_FOREACH_END();
} /* }}} */

/* Checks if tags is valid to set on a mongoc_read_prefs_t. It may be null or an
 * array of one or more documents. */
bool php_phongo_read_preference_tags_are_valid(const bson_t* tags) /* {{{ */
{
	bson_iter_t iter;

	if (bson_empty0(tags)) {
		return true;
	}

	if (!bson_iter_init(&iter, tags)) {
		return false;
	}

	while (bson_iter_next(&iter)) {
		if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
			return false;
		}
	}

	return true;
} /* }}} */

void php_phongo_write_concern_to_zval(zval* retval, const mongoc_write_concern_t* write_concern) /* {{{ */
{
	const char*   wtag     = mongoc_write_concern_get_wtag(write_concern);
	const int32_t w        = mongoc_write_concern_get_w(write_concern);
	const int64_t wtimeout = mongoc_write_concern_get_wtimeout_int64(write_concern);

	array_init_size(retval, 4);

	if (wtag) {
		ADD_ASSOC_STRING(retval, "w", wtag);
	} else if (mongoc_write_concern_get_wmajority(write_concern)) {
		ADD_ASSOC_STRING(retval, "w", PHONGO_WRITE_CONCERN_W_MAJORITY);
	} else if (w != MONGOC_WRITE_CONCERN_W_DEFAULT) {
		ADD_ASSOC_LONG_EX(retval, "w", w);
	}

	if (mongoc_write_concern_journal_is_set(write_concern)) {
		ADD_ASSOC_BOOL_EX(retval, "j", mongoc_write_concern_get_journal(write_concern));
	}

	if (wtimeout != 0) {
#if SIZEOF_ZEND_LONG == 4
		if (wtimeout > INT32_MAX || wtimeout < INT32_MIN) {
			ADD_ASSOC_INT64_AS_STRING(&retval, "wtimeout", wtimeout);
		} else {
			ADD_ASSOC_LONG_EX(retval, "wtimeout", wtimeout);
		}
#else
		ADD_ASSOC_LONG_EX(retval, "wtimeout", wtimeout);
#endif
	}
} /* }}} */
/* }}} */

static mongoc_uri_t* php_phongo_make_uri(const char* uri_string) /* {{{ */
{
	mongoc_uri_t* uri;
	bson_error_t  error = { 0 };

	uri = mongoc_uri_new_with_error(uri_string, &error);
	MONGOC_DEBUG("Connection string: '%s'", uri_string);

	if (!uri) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse MongoDB URI: '%s'. %s.", uri_string, error.message);
		return NULL;
	}

	return uri;
} /* }}} */

static const char* php_phongo_bson_type_to_string(bson_type_t type) /* {{{ */
{
	switch (type) {
		case BSON_TYPE_EOD:
			return "EOD";
		case BSON_TYPE_DOUBLE:
			return "double";
		case BSON_TYPE_UTF8:
			return "string";
		case BSON_TYPE_DOCUMENT:
			return "document";
		case BSON_TYPE_ARRAY:
			return "array";
		case BSON_TYPE_BINARY:
			return "Binary";
		case BSON_TYPE_UNDEFINED:
			return "undefined";
		case BSON_TYPE_OID:
			return "ObjectId";
		case BSON_TYPE_BOOL:
			return "boolean";
		case BSON_TYPE_DATE_TIME:
			return "UTCDateTime";
		case BSON_TYPE_NULL:
			return "null";
		case BSON_TYPE_REGEX:
			return "Regex";
		case BSON_TYPE_DBPOINTER:
			return "DBPointer";
		case BSON_TYPE_CODE:
			return "Javascript";
		case BSON_TYPE_SYMBOL:
			return "symbol";
		case BSON_TYPE_CODEWSCOPE:
			return "Javascript with scope";
		case BSON_TYPE_INT32:
			return "32-bit integer";
		case BSON_TYPE_TIMESTAMP:
			return "Timestamp";
		case BSON_TYPE_INT64:
			return "64-bit integer";
		case BSON_TYPE_DECIMAL128:
			return "Decimal128";
		case BSON_TYPE_MAXKEY:
			return "MaxKey";
		case BSON_TYPE_MINKEY:
			return "MinKey";
		default:
			return "unknown";
	}
} /* }}} */

#define PHONGO_URI_INVALID_TYPE(iter, expected)        \
	phongo_throw_exception(                            \
		PHONGO_ERROR_INVALID_ARGUMENT,                 \
		"Expected %s for \"%s\" URI option, %s given", \
		(expected),                                    \
		bson_iter_key(&(iter)),                        \
		php_phongo_bson_type_to_string(bson_iter_type(&(iter))))

static bool php_phongo_uri_finalize_auth(mongoc_uri_t* uri) /* {{{ */
{
	const bson_t* credentials = mongoc_uri_get_credentials(uri);
	bson_iter_t   iter;
	const char*   source       = NULL;
	const char*   username     = mongoc_uri_get_username(uri);
	bool          require_auth = username != NULL;

	if (bson_iter_init_find_case(&iter, credentials, MONGOC_URI_AUTHSOURCE)) {
		source       = bson_iter_utf8(&iter, NULL);
		require_auth = true;
	}

	/* authSource with GSSAPI or X509 should always be external */
	if (mongoc_uri_get_auth_mechanism(uri)) {
		if (!strcasecmp(mongoc_uri_get_auth_mechanism(uri), "GSSAPI") ||
			!strcasecmp(mongoc_uri_get_auth_mechanism(uri), "MONGODB-X509")) {

			if (source) {
				if (strcasecmp(source, "$external")) {
					phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse URI options: GSSAPI and X509 require \"$external\" authSource.");
					return false;
				}
			} else {
				mongoc_uri_set_auth_source(uri, "$external");
			}
		}

		/* Mechanisms other than MONGODB-X509 and MONGODB-AWS require a username */
		if (strcasecmp(mongoc_uri_get_auth_mechanism(uri), "MONGODB-X509") &&
			strcasecmp(mongoc_uri_get_auth_mechanism(uri), "MONGODB-AWS")) {
			if (!mongoc_uri_get_username(uri) ||
				!strcmp(mongoc_uri_get_username(uri), "")) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse URI options: '%s' authentication mechanism requires username.", mongoc_uri_get_auth_mechanism(uri));
				return false;
			}
		}

		/* MONGODB-X509 errors if a password is supplied. */
		if (!strcasecmp(mongoc_uri_get_auth_mechanism(uri), "MONGODB-X509")) {
			if (mongoc_uri_get_password(uri)) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse URI options: X509 authentication mechanism does not accept a password.");
				return false;
			}
		}
	} else if (require_auth) {
		if (source && strcmp(source, "$external") != 0 && (!username || strcmp(username, "") == 0)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse URI options: Default authentication mechanism requires username.");
			return false;
		}
	}

	return true;
} /* }}} */

static bool php_phongo_uri_finalize_directconnection(mongoc_uri_t* uri) /* {{{ */
{
	const mongoc_host_list_t* hosts;

	if (!mongoc_uri_get_option_as_bool(uri, MONGOC_URI_DIRECTCONNECTION, false)) {
		return true;
	}

	/* Per the URI options spec, directConnection conflicts with multiple hosts
	 * and SRV URIs, which may resolve to multiple hosts. */
	if (!strncmp(mongoc_uri_get_string(uri), "mongodb+srv://", 14)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse URI options: SRV URI not allowed with directConnection option.");
		return false;
	}

	hosts = mongoc_uri_get_hosts(uri);

	if (hosts && hosts->next) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse URI options: Multiple seeds not allowed with directConnection option.");
		return false;
	}

	return true;
} /* }}} */

static bool php_phongo_uri_finalize_tls(mongoc_uri_t* uri) /* {{{ */
{
	const bson_t* options;
	bson_iter_t   iter;

	if (!(options = mongoc_uri_get_options(uri))) {
		return true;
	}

	if (bson_iter_init_find_case(&iter, options, MONGOC_URI_TLSINSECURE) &&
		(bson_iter_init_find_case(&iter, options, MONGOC_URI_TLSALLOWINVALIDCERTIFICATES) ||
		 bson_iter_init_find_case(&iter, options, MONGOC_URI_TLSALLOWINVALIDHOSTNAMES) ||
		 bson_iter_init_find_case(&iter, options, MONGOC_URI_TLSDISABLEOCSPENDPOINTCHECK) ||
		 bson_iter_init_find_case(&iter, options, MONGOC_URI_TLSDISABLECERTIFICATEREVOCATIONCHECK))) {
		phongo_throw_exception(
			PHONGO_ERROR_INVALID_ARGUMENT,
			"Failed to parse URI options: %s may not be combined with %s, %s, %s, or %s.",
			MONGOC_URI_TLSINSECURE,
			MONGOC_URI_TLSALLOWINVALIDCERTIFICATES,
			MONGOC_URI_TLSALLOWINVALIDHOSTNAMES,
			MONGOC_URI_TLSDISABLEOCSPENDPOINTCHECK,
			MONGOC_URI_TLSDISABLECERTIFICATEREVOCATIONCHECK);
		return false;
	}

	if (bson_iter_init_find_case(&iter, options, MONGOC_URI_TLSALLOWINVALIDCERTIFICATES) &&
		(bson_iter_init_find_case(&iter, options, MONGOC_URI_TLSDISABLEOCSPENDPOINTCHECK) ||
		 bson_iter_init_find_case(&iter, options, MONGOC_URI_TLSDISABLECERTIFICATEREVOCATIONCHECK))) {
		phongo_throw_exception(
			PHONGO_ERROR_INVALID_ARGUMENT,
			"Failed to parse URI options: %s may not be combined with %s or %s.",
			MONGOC_URI_TLSALLOWINVALIDCERTIFICATES,
			MONGOC_URI_TLSDISABLEOCSPENDPOINTCHECK,
			MONGOC_URI_TLSDISABLECERTIFICATEREVOCATIONCHECK);
		return false;
	}

	return true;
} /* }}} */

static bool php_phongo_apply_options_to_uri(mongoc_uri_t* uri, bson_t* options) /* {{{ */
{
	bson_iter_t iter;

	/* Return early if there are no options to apply */
	if (bson_empty0(options) || !bson_iter_init(&iter, options)) {
		return true;
	}

	while (bson_iter_next(&iter)) {
		const char* key = bson_iter_key(&iter);

		/* Skip read preference, read concern, and write concern options, as
		 * those will be processed by other functions. */
		if (!strcasecmp(key, MONGOC_URI_JOURNAL) ||
			!strcasecmp(key, MONGOC_URI_MAXSTALENESSSECONDS) ||
			!strcasecmp(key, MONGOC_URI_READCONCERNLEVEL) ||
			!strcasecmp(key, MONGOC_URI_READPREFERENCE) ||
			!strcasecmp(key, MONGOC_URI_READPREFERENCETAGS) ||
			!strcasecmp(key, MONGOC_URI_SAFE) ||
			!strcasecmp(key, MONGOC_URI_SLAVEOK) ||
			!strcasecmp(key, MONGOC_URI_W) ||
			!strcasecmp(key, MONGOC_URI_WTIMEOUTMS)) {

			continue;
		}

		if (mongoc_uri_option_is_bool(key)) {
			/* The option's type is not validated because bson_iter_as_bool() is
			 * used to cast the value to a boolean. Validation may be introduced
			 * in PHPC-990. */
			if (!mongoc_uri_set_option_as_bool(uri, key, bson_iter_as_bool(&iter))) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" URI option", key);
				return false;
			}

			continue;
		}

		if (mongoc_uri_option_is_int32(key)) {
			if (!BSON_ITER_HOLDS_INT32(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "32-bit integer");
				return false;
			}

			if (!mongoc_uri_set_option_as_int32(uri, key, bson_iter_int32(&iter))) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" URI option", key);
				return false;
			}

			continue;
		}

		if (mongoc_uri_option_is_utf8(key)) {
			if (!BSON_ITER_HOLDS_UTF8(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "string");
				return false;
			}

			if (!strcasecmp(key, MONGOC_URI_REPLICASET) && !strcmp("", bson_iter_utf8(&iter, NULL))) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Value for URI option \"%s\" cannot be empty string.", key);
				return false;
			}

			if (!mongoc_uri_set_option_as_utf8(uri, key, bson_iter_utf8(&iter, NULL))) {
				/* Assignment uses mongoc_uri_set_appname() for the "appname"
				 * option, which validates length in addition to UTF-8 encoding.
				 * For BC, we report the invalid string to the user. */
				if (!strcasecmp(key, MONGOC_URI_APPNAME)) {
					phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Invalid appname value: '%s'", bson_iter_utf8(&iter, NULL));
				} else {
					phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" URI option", key);
				}
				return false;
			}

			continue;
		}

		if (!strcasecmp(key, "username")) {
			if (!BSON_ITER_HOLDS_UTF8(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "string");
				return false;
			}

			if (!mongoc_uri_set_username(uri, bson_iter_utf8(&iter, NULL))) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" URI option", key);
				return false;
			}

			continue;
		}

		if (!strcasecmp(key, "password")) {
			if (!BSON_ITER_HOLDS_UTF8(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "string");
				return false;
			}

			if (!mongoc_uri_set_password(uri, bson_iter_utf8(&iter, NULL))) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" URI option", key);
				return false;
			}

			continue;
		}

		if (!strcasecmp(key, MONGOC_URI_AUTHMECHANISM)) {
			if (!BSON_ITER_HOLDS_UTF8(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "string");
				return false;
			}

			if (!mongoc_uri_set_auth_mechanism(uri, bson_iter_utf8(&iter, NULL))) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" URI option", key);
				return false;
			}

			continue;
		}

		if (!strcasecmp(key, MONGOC_URI_AUTHSOURCE)) {
			if (!BSON_ITER_HOLDS_UTF8(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "string");
				return false;
			}

			if (!mongoc_uri_set_auth_source(uri, bson_iter_utf8(&iter, NULL))) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" URI option", key);
				return false;
			}

			continue;
		}

		if (!strcasecmp(key, MONGOC_URI_AUTHMECHANISMPROPERTIES)) {
			bson_t         properties;
			uint32_t       len;
			const uint8_t* data;

			if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "array or object");
				return false;
			}

			bson_iter_document(&iter, &len, &data);

			if (!bson_init_static(&properties, data, len)) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Could not initialize BSON structure for auth mechanism properties");
				return false;
			}

			if (!mongoc_uri_set_mechanism_properties(uri, &properties)) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" URI option", key);
				return false;
			}

			continue;
		}

		if (!strcasecmp(key, MONGOC_URI_GSSAPISERVICENAME)) {
			bson_t unused, properties = BSON_INITIALIZER;

			if (mongoc_uri_get_mechanism_properties(uri, &unused)) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "authMechanismProperties SERVICE_NAME already set, ignoring \"%s\"", key);
				return false;
			}

			if (!BSON_ITER_HOLDS_UTF8(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "string");
				return false;
			}

			bson_append_utf8(&properties, "SERVICE_NAME", -1, bson_iter_utf8(&iter, NULL), -1);

			if (!mongoc_uri_set_mechanism_properties(uri, &properties)) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" URI option", key);
				bson_destroy(&properties);
				return false;
			}

			bson_destroy(&properties);

			continue;
		}

		if (!strcasecmp(key, MONGOC_URI_COMPRESSORS)) {
			if (!BSON_ITER_HOLDS_UTF8(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "string");
				return false;
			}

			if (!mongoc_uri_set_compressors(uri, bson_iter_utf8(&iter, NULL))) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" URI option", key);
				return false;
			}

			continue;
		}
	}

	/* Validate any interactions between URI options */
	if (!php_phongo_uri_finalize_auth(uri)) {
		/* Exception should already have been thrown */
		return false;
	}

	if (!php_phongo_uri_finalize_directconnection(uri)) {
		/* Exception should already have been thrown */
		return false;
	}

	return true;
} /* }}} */

static bool php_phongo_apply_rc_options_to_uri(mongoc_uri_t* uri, bson_t* options) /* {{{ */
{
	bson_iter_t                  iter;
	mongoc_read_concern_t*       new_rc;
	const mongoc_read_concern_t* old_rc;

	if (!(old_rc = mongoc_uri_get_read_concern(uri))) {
		phongo_throw_exception(PHONGO_ERROR_MONGOC_FAILED, "mongoc_uri_t does not have a read concern");

		return false;
	}

	/* Return early if there are no options to apply */
	if (bson_empty0(options) || !bson_iter_init(&iter, options)) {
		return true;
	}

	new_rc = mongoc_read_concern_copy(old_rc);

	while (bson_iter_next(&iter)) {
		const char* key = bson_iter_key(&iter);

		if (!strcasecmp(key, MONGOC_URI_READCONCERNLEVEL)) {
			if (!BSON_ITER_HOLDS_UTF8(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "string");
				mongoc_read_concern_destroy(new_rc);

				return false;
			}

			mongoc_read_concern_set_level(new_rc, bson_iter_utf8(&iter, NULL));
		}
	}

	mongoc_uri_set_read_concern(uri, new_rc);
	mongoc_read_concern_destroy(new_rc);

	return true;
} /* }}} */

static bool php_phongo_apply_rp_options_to_uri(mongoc_uri_t* uri, bson_t* options) /* {{{ */
{
	bson_iter_t                iter;
	mongoc_read_prefs_t*       new_rp;
	const mongoc_read_prefs_t* old_rp;
	bool                       ignore_slaveok = false;

	if (!(old_rp = mongoc_uri_get_read_prefs_t(uri))) {
		phongo_throw_exception(PHONGO_ERROR_MONGOC_FAILED, "mongoc_uri_t does not have a read preference");

		return false;
	}

	/* Return early if there are no options to apply */
	if (bson_empty0(options) || !bson_iter_init(&iter, options)) {
		return true;
	}

	new_rp = mongoc_read_prefs_copy(old_rp);

	while (bson_iter_next(&iter)) {
		const char* key = bson_iter_key(&iter);

		if (!ignore_slaveok && !strcasecmp(key, MONGOC_URI_SLAVEOK)) {
			if (!BSON_ITER_HOLDS_BOOL(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "boolean");
				mongoc_read_prefs_destroy(new_rp);

				return false;
			}

			if (bson_iter_bool(&iter)) {
				mongoc_read_prefs_set_mode(new_rp, MONGOC_READ_SECONDARY_PREFERRED);
			}
		}

		if (!strcasecmp(key, MONGOC_URI_READPREFERENCE)) {
			const char* str;

			if (!BSON_ITER_HOLDS_UTF8(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "string");
				mongoc_read_prefs_destroy(new_rp);

				return false;
			}

			str = bson_iter_utf8(&iter, NULL);

			if (0 == strcasecmp("primary", str)) {
				mongoc_read_prefs_set_mode(new_rp, MONGOC_READ_PRIMARY);
			} else if (0 == strcasecmp("primarypreferred", str)) {
				mongoc_read_prefs_set_mode(new_rp, MONGOC_READ_PRIMARY_PREFERRED);
			} else if (0 == strcasecmp("secondary", str)) {
				mongoc_read_prefs_set_mode(new_rp, MONGOC_READ_SECONDARY);
			} else if (0 == strcasecmp("secondarypreferred", str)) {
				mongoc_read_prefs_set_mode(new_rp, MONGOC_READ_SECONDARY_PREFERRED);
			} else if (0 == strcasecmp("nearest", str)) {
				mongoc_read_prefs_set_mode(new_rp, MONGOC_READ_NEAREST);
			} else {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Unsupported %s value: '%s'", bson_iter_key(&iter), str);
				mongoc_read_prefs_destroy(new_rp);

				return false;
			}

			ignore_slaveok = true;
		}

		if (!strcasecmp(key, MONGOC_URI_READPREFERENCETAGS)) {
			bson_t         tags;
			uint32_t       len;
			const uint8_t* data;

			if (!BSON_ITER_HOLDS_ARRAY(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "array");
				mongoc_read_prefs_destroy(new_rp);

				return false;
			}

			bson_iter_array(&iter, &len, &data);

			if (!bson_init_static(&tags, data, len)) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Could not initialize BSON structure for read preference tags");
				mongoc_read_prefs_destroy(new_rp);

				return false;
			}

			if (!php_phongo_read_preference_tags_are_valid(&tags)) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Read preference tags must be an array of zero or more documents");
				mongoc_read_prefs_destroy(new_rp);

				return false;
			}

			mongoc_read_prefs_set_tags(new_rp, &tags);
		}

		if (!strcasecmp(key, MONGOC_URI_MAXSTALENESSSECONDS)) {
			int64_t max_staleness_seconds;

			if (!BSON_ITER_HOLDS_INT(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "integer");
				mongoc_read_prefs_destroy(new_rp);

				return false;
			}

			max_staleness_seconds = bson_iter_as_int64(&iter);

			if (max_staleness_seconds != MONGOC_NO_MAX_STALENESS) {

				if (max_staleness_seconds < MONGOC_SMALLEST_MAX_STALENESS_SECONDS) {
					phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected maxStalenessSeconds to be >= %d, %" PRId64 " given", MONGOC_SMALLEST_MAX_STALENESS_SECONDS, max_staleness_seconds);
					mongoc_read_prefs_destroy(new_rp);

					return false;
				}

				if (max_staleness_seconds > INT32_MAX) {
					phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected maxStalenessSeconds to be <= %d, %" PRId64 " given", INT32_MAX, max_staleness_seconds);
					mongoc_read_prefs_destroy(new_rp);

					return false;
				}

				if (mongoc_read_prefs_get_mode(new_rp) == MONGOC_READ_PRIMARY) {
					phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Primary read preference mode conflicts with maxStalenessSeconds");
					mongoc_read_prefs_destroy(new_rp);

					return false;
				}
			}

			mongoc_read_prefs_set_max_staleness_seconds(new_rp, max_staleness_seconds);
		}
	}

	if (mongoc_read_prefs_get_mode(new_rp) == MONGOC_READ_PRIMARY &&
		!bson_empty(mongoc_read_prefs_get_tags(new_rp))) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Primary read preference mode conflicts with tags");
		mongoc_read_prefs_destroy(new_rp);

		return false;
	}

	/* Make sure maxStalenessSeconds is not combined with primary readPreference */
	if (mongoc_read_prefs_get_mode(new_rp) == MONGOC_READ_PRIMARY &&
		mongoc_read_prefs_get_max_staleness_seconds(new_rp) != MONGOC_NO_MAX_STALENESS) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Primary read preference mode conflicts with maxStalenessSeconds");
		mongoc_read_prefs_destroy(new_rp);

		return false;
	}

	/* This may be redundant in light of the previous checks (primary with tags
	 * or maxStalenessSeconds), but we'll check anyway in case additional
	 * validation is implemented. */
	if (!mongoc_read_prefs_is_valid(new_rp)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Read preference is not valid");
		mongoc_read_prefs_destroy(new_rp);

		return false;
	}

	mongoc_uri_set_read_prefs_t(uri, new_rp);
	mongoc_read_prefs_destroy(new_rp);

	return true;
} /* }}} */

static bool php_phongo_apply_wc_options_to_uri(mongoc_uri_t* uri, bson_t* options) /* {{{ */
{
	bson_iter_t                   iter;
	mongoc_write_concern_t*       new_wc;
	const mongoc_write_concern_t* old_wc;
	bool                          ignore_safe = false;

	if (!(old_wc = mongoc_uri_get_write_concern(uri))) {
		phongo_throw_exception(PHONGO_ERROR_MONGOC_FAILED, "mongoc_uri_t does not have a write concern");

		return false;
	}

	/* Return early if there are no options to apply */
	if (bson_empty0(options) || !bson_iter_init(&iter, options)) {
		return true;
	}

	new_wc = mongoc_write_concern_copy(old_wc);

	while (bson_iter_next(&iter)) {
		const char* key = bson_iter_key(&iter);

		if (!ignore_safe && !strcasecmp(key, MONGOC_URI_SAFE)) {
			if (!BSON_ITER_HOLDS_BOOL(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "boolean");
				mongoc_write_concern_destroy(new_wc);

				return false;
			}

			mongoc_write_concern_set_w(new_wc, bson_iter_bool(&iter) ? 1 : MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED);
		}

		if (!strcasecmp(key, MONGOC_URI_WTIMEOUTMS)) {
			int64_t wtimeout;

			if (!BSON_ITER_HOLDS_INT(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "integer");
				mongoc_write_concern_destroy(new_wc);

				return false;
			}

			wtimeout = bson_iter_as_int64(&iter);

			if (wtimeout < 0) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected wtimeoutMS to be >= 0, %" PRId64 " given", wtimeout);
				mongoc_write_concern_destroy(new_wc);

				return false;
			}

			mongoc_write_concern_set_wtimeout_int64(new_wc, wtimeout);
		}

		if (!strcasecmp(key, MONGOC_URI_JOURNAL)) {
			if (!BSON_ITER_HOLDS_BOOL(&iter)) {
				PHONGO_URI_INVALID_TYPE(iter, "boolean");
				mongoc_write_concern_destroy(new_wc);

				return false;
			}

			mongoc_write_concern_set_journal(new_wc, bson_iter_bool(&iter));
		}

		if (!strcasecmp(key, MONGOC_URI_W)) {
			if (BSON_ITER_HOLDS_INT32(&iter)) {
				int32_t value = bson_iter_int32(&iter);

				switch (value) {
					case MONGOC_WRITE_CONCERN_W_ERRORS_IGNORED:
					case MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED:
						mongoc_write_concern_set_w(new_wc, value);
						break;

					default:
						if (value > 0) {
							mongoc_write_concern_set_w(new_wc, value);
							break;
						}
						phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Unsupported w value: %d", value);
						mongoc_write_concern_destroy(new_wc);

						return false;
				}
			} else if (BSON_ITER_HOLDS_UTF8(&iter)) {
				const char* str = bson_iter_utf8(&iter, NULL);

				if (0 == strcasecmp(PHONGO_WRITE_CONCERN_W_MAJORITY, str)) {
					mongoc_write_concern_set_w(new_wc, MONGOC_WRITE_CONCERN_W_MAJORITY);
				} else {
					mongoc_write_concern_set_wtag(new_wc, str);
				}
			} else {
				PHONGO_URI_INVALID_TYPE(iter, "32-bit integer or string");
				mongoc_write_concern_destroy(new_wc);

				return false;
			}

			ignore_safe = true;
		}
	}

	if (mongoc_write_concern_get_journal(new_wc)) {
		int32_t w = mongoc_write_concern_get_w(new_wc);

		if (w == MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED || w == MONGOC_WRITE_CONCERN_W_ERRORS_IGNORED) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Journal conflicts with w value: %d", w);
			mongoc_write_concern_destroy(new_wc);

			return false;
		}
	}

	/* This may be redundant in light of the last check (unacknowledged w with
	   journal), but we'll check anyway in case additional validation is
	   implemented. */
	if (!mongoc_write_concern_is_valid(new_wc)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Write concern is not valid");
		mongoc_write_concern_destroy(new_wc);

		return false;
	}

	mongoc_uri_set_write_concern(uri, new_wc);
	mongoc_write_concern_destroy(new_wc);

	return true;
} /* }}} */

#ifdef MONGOC_ENABLE_SSL

static void php_phongo_mongoc_ssl_opts_from_uri(mongoc_ssl_opt_t* ssl_opt, mongoc_uri_t* uri, bool* any_ssl_option_set)
{
	bool        insecure = mongoc_uri_get_option_as_bool(uri, MONGOC_URI_TLSINSECURE, false);
	const char* pem_file = mongoc_uri_get_option_as_utf8(uri, MONGOC_URI_TLSCERTIFICATEKEYFILE, NULL);
	const char* pem_pwd  = mongoc_uri_get_option_as_utf8(uri, MONGOC_URI_TLSCERTIFICATEKEYFILEPASSWORD, NULL);
	const char* ca_file  = mongoc_uri_get_option_as_utf8(uri, MONGOC_URI_TLSCAFILE, NULL);

	ssl_opt->pem_file               = pem_file ? estrdup(pem_file) : NULL;
	ssl_opt->pem_pwd                = pem_pwd ? estrdup(pem_pwd) : NULL;
	ssl_opt->ca_file                = ca_file ? estrdup(ca_file) : NULL;
	ssl_opt->weak_cert_validation   = mongoc_uri_get_option_as_bool(uri, MONGOC_URI_TLSALLOWINVALIDCERTIFICATES, insecure);
	ssl_opt->allow_invalid_hostname = mongoc_uri_get_option_as_bool(uri, MONGOC_URI_TLSALLOWINVALIDHOSTNAMES, insecure);

	/* Boolean options default to false, so we cannot consider them for
	 * any_ssl_option_set. This isn't actually a problem as libmongoc will
	 * already have assigned them when creating the client, enabling SSL, and
	 * assigning SSL options. Therefore, we only need to check for non-defaults
	 * (i.e. non-NULL strings, true booleans). */
	if (pem_file || pem_pwd || ca_file || ssl_opt->weak_cert_validation || ssl_opt->allow_invalid_hostname) {
		*any_ssl_option_set = true;
	}
}

static inline char* php_phongo_fetch_ssl_opt_string(zval* zoptions, const char* key)
{
	int       plen;
	zend_bool pfree;
	char*     pval;
	char*     value;

	pval  = php_array_fetch_string(zoptions, key, &plen, &pfree);
	value = pfree ? pval : estrndup(pval, plen);

	return value;
}

static mongoc_ssl_opt_t* php_phongo_make_ssl_opt(mongoc_uri_t* uri, zval* zoptions)
{
	mongoc_ssl_opt_t* ssl_opt;
	bool              any_ssl_option_set = false;

	if (!zoptions) {
		return NULL;
	}

#if defined(MONGOC_ENABLE_SSL_SECURE_CHANNEL) || defined(MONGOC_ENABLE_SSL_SECURE_TRANSPORT)
	if (php_array_existsc(zoptions, "ca_dir")) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "\"ca_dir\" option is not supported by Secure Channel and Secure Transport");
		return NULL;
	}

	if (php_array_existsc(zoptions, "capath")) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "\"capath\" option is not supported by Secure Channel and Secure Transport");
		return NULL;
	}
#endif

#if defined(MONGOC_ENABLE_SSL_LIBRESSL) || defined(MONGOC_ENABLE_SSL_SECURE_TRANSPORT)
	if (php_array_existsc(zoptions, "crl_file")) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "\"crl_file\" option is not supported by LibreSSL and Secure Transport");
		return NULL;
	}
#endif

	ssl_opt = ecalloc(1, sizeof(mongoc_ssl_opt_t));

	/* If SSL options are set in the URL, we need to read them and set them on
	 * the options struct so we can merge potential options from passed in
	 * driverOptions (zoptions) */
	if (mongoc_uri_get_tls(uri)) {
		php_phongo_mongoc_ssl_opts_from_uri(ssl_opt, uri, &any_ssl_option_set);
	}

#define PHONGO_SSL_OPTION_SWAP_STRING(o, n) \
	if ((o)) {                              \
		efree((char*) (o));                 \
	}                                       \
	(o) = php_phongo_fetch_ssl_opt_string(zoptions, n);

	/* Apply driver options that don't have a corresponding URI option. These
	 * are set directly on the SSL options struct. */
	if (php_array_existsc(zoptions, "ca_dir")) {
		PHONGO_SSL_OPTION_SWAP_STRING(ssl_opt->ca_dir, "ca_dir");
		any_ssl_option_set = true;
	} else if (php_array_existsc(zoptions, "capath")) {
		PHONGO_SSL_OPTION_SWAP_STRING(ssl_opt->ca_dir, "capath");
		any_ssl_option_set = true;

		php_error_docref(NULL, E_DEPRECATED, "The \"capath\" context driver option is deprecated. Please use the \"ca_dir\" driver option instead.");
	}

	if (php_array_existsc(zoptions, "crl_file")) {
		PHONGO_SSL_OPTION_SWAP_STRING(ssl_opt->crl_file, "crl_file");
		any_ssl_option_set = true;
	}

#undef PHONGO_SSL_OPTION_SWAP_STRING

	if (!any_ssl_option_set) {
		efree(ssl_opt);
		return NULL;
	}

	return ssl_opt;
}

static void php_phongo_free_ssl_opt(mongoc_ssl_opt_t* ssl_opt)
{
	if (ssl_opt->pem_file) {
		efree((char*) ssl_opt->pem_file);
	}

	if (ssl_opt->pem_pwd) {
		efree((char*) ssl_opt->pem_pwd);
	}

	if (ssl_opt->ca_file) {
		efree((char*) ssl_opt->ca_file);
	}

	if (ssl_opt->ca_dir) {
		efree((char*) ssl_opt->ca_dir);
	}

	if (ssl_opt->crl_file) {
		efree((char*) ssl_opt->crl_file);
	}

	efree(ssl_opt);
}

static inline bool php_phongo_apply_driver_option_to_uri(mongoc_uri_t* uri, zval* zoptions, const char* driverOptionKey, const char* optionKey)
{
	bool  ret;
	char* value;

	value = php_phongo_fetch_ssl_opt_string(zoptions, driverOptionKey);
	ret   = mongoc_uri_set_option_as_utf8(uri, optionKey, value);
	efree(value);

	return ret;
}

static bool php_phongo_apply_driver_options_to_uri(mongoc_uri_t* uri, zval* zoptions)
{
	if (!zoptions) {
		return true;
	}

	/* Map TLS driver options to the canonical tls options in the URI. */
	if (php_array_existsc(zoptions, "allow_invalid_hostname")) {
		if (!mongoc_uri_set_option_as_bool(uri, MONGOC_URI_TLSALLOWINVALIDHOSTNAMES, php_array_fetchc_bool(zoptions, "allow_invalid_hostname"))) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" driver option", "allow_invalid_hostname");

			return false;
		}

		php_error_docref(NULL, E_DEPRECATED, "The \"allow_invalid_hostname\" driver option is deprecated. Please use the \"tlsAllowInvalidHostnames\" URI option instead.");
	}

	if (php_array_existsc(zoptions, "weak_cert_validation")) {
		if (!mongoc_uri_set_option_as_bool(uri, MONGOC_URI_TLSALLOWINVALIDCERTIFICATES, php_array_fetchc_bool(zoptions, "weak_cert_validation"))) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" driver option", "weak_cert_validation");

			return false;
		}

		php_error_docref(NULL, E_DEPRECATED, "The \"weak_cert_validation\" driver option is deprecated. Please use the \"tlsAllowInvalidCertificates\" URI option instead.");
	} else if (php_array_existsc(zoptions, "allow_self_signed")) {
		if (!mongoc_uri_set_option_as_bool(uri, MONGOC_URI_TLSALLOWINVALIDCERTIFICATES, php_array_fetchc_bool(zoptions, "allow_self_signed"))) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" driver option", "allow_self_signed");

			return false;
		}

		php_error_docref(NULL, E_DEPRECATED, "The \"allow_self_signed\" context driver option is deprecated. Please use the \"tlsAllowInvalidCertificates\" URI option instead.");
	}

	if (php_array_existsc(zoptions, "pem_file")) {
		if (!php_phongo_apply_driver_option_to_uri(uri, zoptions, "pem_file", MONGOC_URI_TLSCERTIFICATEKEYFILE)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" driver option", "pem_file");

			return false;
		}

		php_error_docref(NULL, E_DEPRECATED, "The \"pem_file\" driver option is deprecated. Please use the \"tlsCertificateKeyFile\" URI option instead.");
	} else if (php_array_existsc(zoptions, "local_cert")) {
		if (!php_phongo_apply_driver_option_to_uri(uri, zoptions, "local_cert", MONGOC_URI_TLSCERTIFICATEKEYFILE)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" driver option", "local_cert");

			return false;
		}

		php_error_docref(NULL, E_DEPRECATED, "The \"local_cert\" context driver option is deprecated. Please use the \"tlsCertificateKeyFile\" URI option instead.");
	}

	if (php_array_existsc(zoptions, "pem_pwd")) {
		if (!php_phongo_apply_driver_option_to_uri(uri, zoptions, "pem_pwd", MONGOC_URI_TLSCERTIFICATEKEYFILEPASSWORD)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" driver option", "pem_pwd");

			return false;
		}

		php_error_docref(NULL, E_DEPRECATED, "The \"pem_pwd\" driver option is deprecated. Please use the \"tlsCertificateKeyFilePassword\" URI option instead.");
	} else if (php_array_existsc(zoptions, "passphrase")) {
		if (!php_phongo_apply_driver_option_to_uri(uri, zoptions, "passphrase", MONGOC_URI_TLSCERTIFICATEKEYFILEPASSWORD)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" driver option", "passphrase");

			return false;
		}

		php_error_docref(NULL, E_DEPRECATED, "The \"passphrase\" context driver option is deprecated. Please use the \"tlsCertificateKeyFilePassword\" URI option instead.");
	}

	if (php_array_existsc(zoptions, "ca_file")) {
		if (!php_phongo_apply_driver_option_to_uri(uri, zoptions, "ca_file", MONGOC_URI_TLSCAFILE)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" driver option", "ca_file");

			return false;
		}

		php_error_docref(NULL, E_DEPRECATED, "The \"ca_file\" driver option is deprecated. Please use the \"tlsCAFile\" URI option instead.");
	} else if (php_array_existsc(zoptions, "cafile")) {
		if (!php_phongo_apply_driver_option_to_uri(uri, zoptions, "cafile", MONGOC_URI_TLSCAFILE)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Failed to parse \"%s\" driver option", "cafile");

			return false;
		}

		php_error_docref(NULL, E_DEPRECATED, "The \"cafile\" context driver option is deprecated. Please use the \"tlsCAFile\" URI option instead.");
	}

	return true;
}
#endif

/* APM callbacks */
static void php_phongo_dispatch_handlers(const char* name, zval* z_event)
{
	zval* value;

	ZEND_HASH_FOREACH_VAL_IND(MONGODB_G(subscribers), value)
	{
		if (EG(exception)) {
			break;
		}
		/* We can't use the zend_call_method_with_1_params macro here, as it
		 * does a sizeof() on the name argument, which does only work with
		 * constant names, but not with parameterized ones as it does
		 * "sizeof(char*)" in that case. */
		zend_call_method(PHONGO_COMPAT_OBJ_P(value), NULL, NULL, name, strlen(name), NULL, 1, z_event, NULL);
	}
	ZEND_HASH_FOREACH_END();
}

/* Search for a Manager associated with the given client in the request-scoped
 * registry. If any Manager is found, copy it into the output parameter
 * (incrementing its ref-count) and return true; otherwise, set the output
 * parameter to undefined and return false. */
static bool php_phongo_copy_manager_for_client(mongoc_client_t* client, zval* out)
{
	php_phongo_manager_t* manager;

	if (!MONGODB_G(managers) || zend_hash_num_elements(MONGODB_G(managers)) == 0) {
		return false;
	}

	ZEND_HASH_FOREACH_PTR(MONGODB_G(managers), manager)
	{
		if (manager->client == client) {
			ZVAL_OBJ(out, &manager->std);
			Z_ADDREF_P(out);

			return true;
		}
	}
	ZEND_HASH_FOREACH_END();

	ZVAL_UNDEF(out);

	return false;
}

static void php_phongo_command_started(const mongoc_apm_command_started_t* event)
{
	php_phongo_commandstartedevent_t* p_event;
	zval                              z_event;

	/* Return early if there are no APM subscribers to notify */
	if (!MONGODB_G(subscribers) || zend_hash_num_elements(MONGODB_G(subscribers)) == 0) {
		return;
	}

	object_init_ex(&z_event, php_phongo_commandstartedevent_ce);
	p_event = Z_COMMANDSTARTEDEVENT_OBJ_P(&z_event);

	p_event->command_name  = estrdup(mongoc_apm_command_started_get_command_name(event));
	p_event->server_id     = mongoc_apm_command_started_get_server_id(event);
	p_event->operation_id  = mongoc_apm_command_started_get_operation_id(event);
	p_event->request_id    = mongoc_apm_command_started_get_request_id(event);
	p_event->command       = bson_copy(mongoc_apm_command_started_get_command(event));
	p_event->database_name = estrdup(mongoc_apm_command_started_get_database_name(event));

	if (!php_phongo_copy_manager_for_client(mongoc_apm_command_started_get_context(event), &p_event->manager)) {
		phongo_throw_exception(PHONGO_ERROR_UNEXPECTED_VALUE, "Found no Manager for client in APM event context");
		zval_ptr_dtor(&z_event);

		return;
	}

	php_phongo_dispatch_handlers("commandStarted", &z_event);
	zval_ptr_dtor(&z_event);
}

static void php_phongo_command_succeeded(const mongoc_apm_command_succeeded_t* event)
{
	php_phongo_commandsucceededevent_t* p_event;
	zval                                z_event;

	/* Return early if there are no APM subscribers to notify */
	if (!MONGODB_G(subscribers) || zend_hash_num_elements(MONGODB_G(subscribers)) == 0) {
		return;
	}

	object_init_ex(&z_event, php_phongo_commandsucceededevent_ce);
	p_event = Z_COMMANDSUCCEEDEDEVENT_OBJ_P(&z_event);

	p_event->command_name    = estrdup(mongoc_apm_command_succeeded_get_command_name(event));
	p_event->server_id       = mongoc_apm_command_succeeded_get_server_id(event);
	p_event->operation_id    = mongoc_apm_command_succeeded_get_operation_id(event);
	p_event->request_id      = mongoc_apm_command_succeeded_get_request_id(event);
	p_event->duration_micros = mongoc_apm_command_succeeded_get_duration(event);
	p_event->reply           = bson_copy(mongoc_apm_command_succeeded_get_reply(event));

	if (!php_phongo_copy_manager_for_client(mongoc_apm_command_succeeded_get_context(event), &p_event->manager)) {
		phongo_throw_exception(PHONGO_ERROR_UNEXPECTED_VALUE, "Found no Manager for client in APM event context");
		zval_ptr_dtor(&z_event);

		return;
	}

	php_phongo_dispatch_handlers("commandSucceeded", &z_event);
	zval_ptr_dtor(&z_event);
}

static void php_phongo_command_failed(const mongoc_apm_command_failed_t* event)
{
	php_phongo_commandfailedevent_t* p_event;
	zval                             z_event;
	bson_error_t                     tmp_error = { 0 };
	zend_class_entry*                default_exception_ce;

	default_exception_ce = zend_exception_get_default();

	/* Return early if there are no APM subscribers to notify */
	if (!MONGODB_G(subscribers) || zend_hash_num_elements(MONGODB_G(subscribers)) == 0) {
		return;
	}

	object_init_ex(&z_event, php_phongo_commandfailedevent_ce);
	p_event = Z_COMMANDFAILEDEVENT_OBJ_P(&z_event);

	p_event->command_name    = estrdup(mongoc_apm_command_failed_get_command_name(event));
	p_event->server_id       = mongoc_apm_command_failed_get_server_id(event);
	p_event->operation_id    = mongoc_apm_command_failed_get_operation_id(event);
	p_event->request_id      = mongoc_apm_command_failed_get_request_id(event);
	p_event->duration_micros = mongoc_apm_command_failed_get_duration(event);
	p_event->reply           = bson_copy(mongoc_apm_command_failed_get_reply(event));

	if (!php_phongo_copy_manager_for_client(mongoc_apm_command_failed_get_context(event), &p_event->manager)) {
		phongo_throw_exception(PHONGO_ERROR_UNEXPECTED_VALUE, "Found no Manager for client in APM event context");
		zval_ptr_dtor(&z_event);

		return;
	}

	/* We need to process and convert the error right here, otherwise
	 * debug_info will turn into a recursive loop, and with the wrong trace
	 * locations */
	mongoc_apm_command_failed_get_error(event, &tmp_error);

	object_init_ex(&p_event->z_error, phongo_exception_from_mongoc_domain(tmp_error.domain, tmp_error.code));
	zend_update_property_string(default_exception_ce, PHONGO_COMPAT_OBJ_P(&p_event->z_error), ZEND_STRL("message"), tmp_error.message);
	zend_update_property_long(default_exception_ce, PHONGO_COMPAT_OBJ_P(&p_event->z_error), ZEND_STRL("code"), tmp_error.code);

	php_phongo_dispatch_handlers("commandFailed", &z_event);
	zval_ptr_dtor(&z_event);
}

/* Sets the callbacks for APM */
bool php_phongo_set_monitoring_callbacks(mongoc_client_t* client)
{
	bool retval;

	mongoc_apm_callbacks_t* callbacks = mongoc_apm_callbacks_new();

	mongoc_apm_set_command_started_cb(callbacks, php_phongo_command_started);
	mongoc_apm_set_command_succeeded_cb(callbacks, php_phongo_command_succeeded);
	mongoc_apm_set_command_failed_cb(callbacks, php_phongo_command_failed);

	retval = mongoc_client_set_apm_callbacks(client, callbacks, client);

	if (!retval) {
		phongo_throw_exception(PHONGO_ERROR_UNEXPECTED_VALUE, "Failed to set APM callbacks");
	}

	mongoc_apm_callbacks_destroy(callbacks);

	return retval;
}

static zval* php_phongo_manager_prepare_manager_for_hash(zval* driverOptions, bool* free)
{
	php_phongo_manager_t* manager;
	zval*                 autoEncryptionOpts      = NULL;
	zval*                 keyVaultClient          = NULL;
	zval*                 driverOptionsClone      = NULL;
	zval*                 autoEncryptionOptsClone = NULL;
	zval                  stackAutoEncryptionOptsClone;

	*free = false;

	if (!driverOptions) {
		return NULL;
	}

	if (!php_array_existsc(driverOptions, "autoEncryption")) {
		goto ref;
	}

	autoEncryptionOpts = php_array_fetchc(driverOptions, "autoEncryption");
	if (Z_TYPE_P(autoEncryptionOpts) != IS_ARRAY) {
		goto ref;
	}

	if (!php_array_existsc(autoEncryptionOpts, "keyVaultClient")) {
		goto ref;
	}

	keyVaultClient = php_array_fetchc(autoEncryptionOpts, "keyVaultClient");
	if (Z_TYPE_P(keyVaultClient) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(keyVaultClient), php_phongo_manager_ce)) {
		goto ref;
	}

	*free = true;

	manager = Z_MANAGER_OBJ_P(keyVaultClient);

	driverOptionsClone      = ecalloc(sizeof(zval), 1);
	autoEncryptionOptsClone = &stackAutoEncryptionOptsClone;

	ZVAL_DUP(autoEncryptionOptsClone, autoEncryptionOpts);
	ADD_ASSOC_STRINGL(autoEncryptionOptsClone, "keyVaultClient", manager->client_hash, manager->client_hash_len);

	ZVAL_DUP(driverOptionsClone, driverOptions);
	ADD_ASSOC_ZVAL_EX(driverOptionsClone, "autoEncryption", autoEncryptionOptsClone);

	return driverOptionsClone;

ref:
	Z_ADDREF_P(driverOptions);
	return driverOptions;
}

/* Creates a hash for a client by concatenating the URI string with serialized
 * options arrays. On success, a persistent string is returned (i.e. pefree()
 * should be used to free it) and hash_len will be set to the string's length.
 * On error, an exception will have been thrown and NULL will be returned. */
static char* php_phongo_manager_make_client_hash(const char* uri_string, zval* options, zval* driverOptions, size_t* hash_len)
{
	char*                hash    = NULL;
	smart_str            var_buf = { 0 };
	php_serialize_data_t var_hash;
	zval*                serializable_driver_options = NULL;
	bool                 free_driver_options         = false;

	zval args;

	array_init_size(&args, 4);
	ADD_ASSOC_LONG_EX(&args, "pid", getpid());
	ADD_ASSOC_STRING(&args, "uri", uri_string);

	if (options) {
		ADD_ASSOC_ZVAL_EX(&args, "options", options);
		Z_ADDREF_P(options);
	} else {
		ADD_ASSOC_NULL_EX(&args, "options");
	}

	if (driverOptions) {
		serializable_driver_options = php_phongo_manager_prepare_manager_for_hash(driverOptions, &free_driver_options);
		ADD_ASSOC_ZVAL_EX(&args, "driverOptions", serializable_driver_options);
	} else {
		ADD_ASSOC_NULL_EX(&args, "driverOptions");
	}

	PHP_VAR_SERIALIZE_INIT(var_hash);
	php_var_serialize(&var_buf, &args, &var_hash);
	PHP_VAR_SERIALIZE_DESTROY(var_hash);

	if (!EG(exception)) {
		*hash_len = ZSTR_LEN(var_buf.s);
		hash      = estrndup(ZSTR_VAL(var_buf.s), *hash_len);
	}

	zval_ptr_dtor(&args);

	if (free_driver_options) {
		efree(serializable_driver_options);
	}

	smart_str_free(&var_buf);

	return hash;
}

static bool php_phongo_extract_handshake_data(zval* driver, const char* key, char** value, size_t* value_len)
{
	zval* zvalue;

	if (!php_array_exists(driver, key)) {
		*value     = NULL;
		*value_len = 0;

		return true;
	}

	zvalue = php_array_fetch(driver, key);

	if (Z_TYPE_P(zvalue) != IS_STRING) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"%s\" handshake option to be a string, %s given", key, PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(zvalue));
		return false;
	}

	*value     = estrdup(Z_STRVAL_P(zvalue));
	*value_len = Z_STRLEN_P(zvalue);

	return true;
}

static char* php_phongo_concat_handshake_data(const char* default_value, const char* custom_value, size_t custom_value_len)
{
	char* ret;
	/* Length of the returned value needs to include the trailing null byte */
	size_t ret_len = strlen(default_value) + 1;

	if (custom_value) {
		/* Increase the length by that of the custom value as well as one byte for the separator */
		ret_len += custom_value_len + PHONGO_METADATA_SEPARATOR_LEN;
	}

	ret = ecalloc(sizeof(char*), ret_len);

	if (custom_value) {
		snprintf(ret, ret_len, "%s%s%s", default_value, PHONGO_METADATA_SEPARATOR, custom_value);
	} else {
		snprintf(ret, ret_len, "%s", default_value);
	}

	return ret;
}

static void php_phongo_handshake_data_append(const char* name, size_t name_len, const char* version, size_t version_len, const char* platform, size_t platform_len)
{
	char*  php_version_string;
	size_t php_version_string_len;
	char*  driver_name;
	char*  driver_version;
	char*  full_platform;

	php_version_string_len = strlen(PHP_VERSION);
	php_version_string     = ecalloc(sizeof(char*), 4 + php_version_string_len);
	snprintf(php_version_string, 4 + php_version_string_len, "PHP %s", PHP_VERSION);

	driver_name    = php_phongo_concat_handshake_data("ext-mongodb:PHP", name, name_len);
	driver_version = php_phongo_concat_handshake_data(PHP_MONGODB_VERSION, version, version_len);
	full_platform  = php_phongo_concat_handshake_data(php_version_string, platform, platform_len);

	MONGOC_DEBUG(
		"Setting driver handshake data: name %s, version %s, platform %s",
		driver_name,
		driver_version,
		full_platform);

	mongoc_handshake_data_append(driver_name, driver_version, full_platform);

	efree(php_version_string);
	efree(driver_name);
	efree(driver_version);
	efree(full_platform);
}

static void php_phongo_set_handshake_data(zval* driverOptions)
{
	char*  name         = NULL;
	size_t name_len     = 0;
	char*  version      = NULL;
	size_t version_len  = 0;
	char*  platform     = NULL;
	size_t platform_len = 0;

	if (driverOptions && php_array_existsc(driverOptions, "driver")) {
		zval* driver = php_array_fetchc(driverOptions, "driver");

		if (Z_TYPE_P(driver) != IS_ARRAY) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"driver\" driver option to be an array, %s given", PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(driver));
			return;
		}

		if (!php_phongo_extract_handshake_data(driver, "name", &name, &name_len)) {
			/* Exception already thrown */
			goto cleanup;
		}

		if (!php_phongo_extract_handshake_data(driver, "version", &version, &version_len)) {
			/* Exception already thrown */
			goto cleanup;
		}

		if (!php_phongo_extract_handshake_data(driver, "platform", &platform, &platform_len)) {
			/* Exception already thrown */
			goto cleanup;
		}
	}

	php_phongo_handshake_data_append(name, name_len, version, version_len, platform, platform_len);

cleanup:
	if (name) {
		efree(name);
	}
	if (version) {
		efree(version);
	}
	if (platform) {
		efree(platform);
	}
}

static mongoc_client_t* php_phongo_make_mongo_client(const mongoc_uri_t* uri, zval* driverOptions) /* {{{ */
{
	const char *mongoc_version, *bson_version;

#ifdef HAVE_SYSTEM_LIBMONGOC
	mongoc_version = mongoc_get_version();
#else
	mongoc_version = "bundled";
#endif

#ifdef HAVE_SYSTEM_LIBBSON
	bson_version = bson_get_version();
#else
	bson_version = "bundled";
#endif

	MONGOC_DEBUG(
		"Creating Manager, phongo-%s[%s] - mongoc-%s(%s), libbson-%s(%s), php-%s",
		PHP_MONGODB_VERSION,
		PHP_MONGODB_STABILITY,
		MONGOC_VERSION_S,
		mongoc_version,
		BSON_VERSION_S,
		bson_version,
		PHP_VERSION);

	php_phongo_set_handshake_data(driverOptions);

	return mongoc_client_new_from_uri(uri);
} /* }}} */

/* Adds a client to the appropriate registry. Persistent and request-scoped
 * clients each have their own registries (i.e. HashTables), which use different
 * forms of memory allocation. Both registries are used for PID tracking.
 * Returns true if the client was successfully added; otherwise, false. */
bool php_phongo_client_register(php_phongo_manager_t* manager)
{
	bool                  is_persistent = manager->use_persistent_client;
	php_phongo_pclient_t* pclient       = pecalloc(1, sizeof(php_phongo_pclient_t), is_persistent);

	pclient->client         = manager->client;
	pclient->created_by_pid = (int) getpid();
	pclient->is_persistent  = is_persistent;

	if (is_persistent) {
		MONGOC_DEBUG("Stored persistent client with hash: %s", manager->client_hash);
		return zend_hash_str_update_ptr(&MONGODB_G(persistent_clients), manager->client_hash, manager->client_hash_len, pclient) != NULL;
	} else {
		MONGOC_DEBUG("Stored non-persistent client");
		return zend_hash_next_index_insert_ptr(MONGODB_G(request_clients), pclient) != NULL;
	}
}

/* Removes a client from the request-scoped registry. This function is a NOP for
 * persistent clients, since they are destroyed along with their registry (i.e.
 * HashTable) in GSHUTDOWN. Returns true if the client was successfully removed;
 * otherwise, false. */
bool php_phongo_client_unregister(php_phongo_manager_t* manager)
{
	zend_ulong            index;
	php_phongo_pclient_t* pclient;

	/* Persistent clients do not get unregistered. */
	if (manager->use_persistent_client) {
		MONGOC_DEBUG("Not destroying persistent client for Manager");

		return false;
	}

	/* Ensure the request-scoped registry is initialized. This is needed because
	 * RSHUTDOWN may occur before a Manager's free_object handler is
	 * executed. */
	if (MONGODB_G(request_clients) == NULL) {
		return false;
	}

	ZEND_HASH_FOREACH_NUM_KEY_PTR(MONGODB_G(request_clients), index, pclient)
	{
		if (pclient->client == manager->client) {
			MONGOC_DEBUG("Destroying non-persistent client for Manager");

			return zend_hash_index_del(MONGODB_G(request_clients), index) == SUCCESS;
		}
	}
	ZEND_HASH_FOREACH_END();

	return false;
}

static mongoc_client_t* php_phongo_find_persistent_client(const char* hash, size_t hash_len)
{
	php_phongo_pclient_t* pclient = zend_hash_str_find_ptr(&MONGODB_G(persistent_clients), hash, hash_len);

	if (pclient) {
		return pclient->client;
	}

	return NULL;
}

#ifdef MONGOC_ENABLE_CLIENT_SIDE_ENCRYPTION
static bool phongo_manager_set_auto_encryption_opts(php_phongo_manager_t* manager, zval* driverOptions) /* {{{ */
{
	zval*                          zAutoEncryptionOpts;
	bson_error_t                   error                = { 0 };
	mongoc_auto_encryption_opts_t* auto_encryption_opts = NULL;
	bool                           retval               = false;

	if (!driverOptions || !php_array_existsc(driverOptions, "autoEncryption")) {
		return true;
	}

	zAutoEncryptionOpts = php_array_fetch(driverOptions, "autoEncryption");

	if (Z_TYPE_P(zAutoEncryptionOpts) != IS_ARRAY) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"autoEncryption\" driver option to be array, %s given", PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(zAutoEncryptionOpts));
		return false;
	}

	auto_encryption_opts = mongoc_auto_encryption_opts_new();

	if (php_array_existsc(zAutoEncryptionOpts, "keyVaultClient")) {
		zval* key_vault_client = php_array_fetch(zAutoEncryptionOpts, "keyVaultClient");

		if (Z_TYPE_P(key_vault_client) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(key_vault_client), php_phongo_manager_ce)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"keyVaultClient\" encryption option to be %s, %s given", ZSTR_VAL(php_phongo_manager_ce->name), PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(key_vault_client));
			goto cleanup;
		}

		/* Ensure the Manager and keyVaultClient are consistent in their use of
		 * persistent clients. A non-persistent Manager could theoretically use
		 * a persistent keyVaultClient, but this prohibition may help prevent
		 * users from inadvertently creating a persistent keyVaultClient. */
		if (manager->use_persistent_client != Z_MANAGER_OBJ_P(key_vault_client)->use_persistent_client) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "The \"disableClientPersistence\" option for a Manager and its \"keyVaultClient\" must be the same");
			goto cleanup;
		}

		mongoc_auto_encryption_opts_set_keyvault_client(auto_encryption_opts, Z_MANAGER_OBJ_P(key_vault_client)->client);

		/* Copy the keyVaultClient to the Manager to allow for ref-counting (for
		 * non-persistent clients) and reset-on-fork behavior. */
		ZVAL_ZVAL(&manager->key_vault_client_manager, key_vault_client, 1, 0);
	}

	if (php_array_existsc(zAutoEncryptionOpts, "keyVaultNamespace")) {
		char*     key_vault_ns;
		char*     db_name;
		char*     coll_name;
		int       plen;
		zend_bool pfree;

		key_vault_ns = php_array_fetch_string(zAutoEncryptionOpts, "keyVaultNamespace", &plen, &pfree);

		if (!phongo_split_namespace(key_vault_ns, &db_name, &coll_name)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"keyVaultNamespace\" encryption option to contain a full collection name");

			if (pfree) {
				efree(key_vault_ns);
			}

			goto cleanup;
		}

		mongoc_auto_encryption_opts_set_keyvault_namespace(auto_encryption_opts, db_name, coll_name);

		efree(db_name);
		efree(coll_name);

		if (pfree) {
			efree(key_vault_ns);
		}
	}

	if (php_array_existsc(zAutoEncryptionOpts, "kmsProviders")) {
		zval*  kms_providers  = php_array_fetch(zAutoEncryptionOpts, "kmsProviders");
		bson_t bson_providers = BSON_INITIALIZER;

		if (Z_TYPE_P(kms_providers) != IS_OBJECT && Z_TYPE_P(kms_providers) != IS_ARRAY) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"kmsProviders\" encryption option to be an array or object");
			goto cleanup;
		}

		php_phongo_zval_to_bson(kms_providers, PHONGO_BSON_NONE, &bson_providers, NULL);
		if (EG(exception)) {
			goto cleanup;
		}

		mongoc_auto_encryption_opts_set_kms_providers(auto_encryption_opts, &bson_providers);

		bson_destroy(&bson_providers);
	}

	if (php_array_existsc(zAutoEncryptionOpts, "schemaMap")) {
		zval*  schema_map = php_array_fetch(zAutoEncryptionOpts, "schemaMap");
		bson_t bson_map   = BSON_INITIALIZER;

		if (Z_TYPE_P(schema_map) != IS_OBJECT && Z_TYPE_P(schema_map) != IS_ARRAY) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"schemaMap\" encryption option to be an array or object");
			goto cleanup;
		}

		php_phongo_zval_to_bson(schema_map, PHONGO_BSON_NONE, &bson_map, NULL);
		if (EG(exception)) {
			goto cleanup;
		}

		mongoc_auto_encryption_opts_set_schema_map(auto_encryption_opts, &bson_map);

		bson_destroy(&bson_map);
	}

	if (php_array_existsc(zAutoEncryptionOpts, "bypassAutoEncryption")) {
		zend_bool bypass_auto_encryption = php_array_fetch_bool(zAutoEncryptionOpts, "bypassAutoEncryption");

		mongoc_auto_encryption_opts_set_bypass_auto_encryption(auto_encryption_opts, bypass_auto_encryption);
	}

	if (php_array_existsc(zAutoEncryptionOpts, "extraOptions")) {
		zval*  extra_options = php_array_fetch(zAutoEncryptionOpts, "extraOptions");
		bson_t bson_options  = BSON_INITIALIZER;

		php_phongo_zval_to_bson(extra_options, PHONGO_BSON_NONE, &bson_options, NULL);
		if (EG(exception)) {
			goto cleanup;
		}

		mongoc_auto_encryption_opts_set_extra(auto_encryption_opts, &bson_options);

		bson_destroy(&bson_options);
	}

	if (!mongoc_client_enable_auto_encryption(manager->client, auto_encryption_opts, &error)) {
		phongo_throw_exception_from_bson_error_t(&error);
		goto cleanup;
	}

	retval = true;

cleanup:
	mongoc_auto_encryption_opts_destroy(auto_encryption_opts);
	return retval;
}
/* }}} */

/* keyVaultClientManager is an output parameter and will be assigned the
 * keyVaultNamespace Manager (if any). */
static mongoc_client_encryption_opts_t* phongo_clientencryption_opts_from_zval(zval* defaultKeyVaultClient, zval* options, zval** keyVaultClientManager) /* {{{ */
{
	mongoc_client_encryption_opts_t* opts;

	opts                   = mongoc_client_encryption_opts_new();
	*keyVaultClientManager = NULL;

	if (!options || Z_TYPE_P(options) != IS_ARRAY) {
		/* Returning opts as-is will defer to mongoc_client_encryption_new to
		 * raise an error for missing required options */
		return opts;
	}

	if (php_array_existsc(options, "keyVaultClient")) {
		zval* key_vault_client = php_array_fetch(options, "keyVaultClient");

		if (Z_TYPE_P(key_vault_client) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(key_vault_client), php_phongo_manager_ce)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"keyVaultClient\" encryption option to be %s, %s given", ZSTR_VAL(php_phongo_manager_ce->name), PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(key_vault_client));
			goto cleanup;
		}

		mongoc_client_encryption_opts_set_keyvault_client(opts, Z_MANAGER_OBJ_P(key_vault_client)->client);
		*keyVaultClientManager = key_vault_client;
	} else {
		mongoc_client_encryption_opts_set_keyvault_client(opts, Z_MANAGER_OBJ_P(defaultKeyVaultClient)->client);
		*keyVaultClientManager = defaultKeyVaultClient;
	}

	if (php_array_existsc(options, "keyVaultNamespace")) {
		char*     keyvault_namespace;
		char*     db_name;
		char*     coll_name;
		int       plen;
		zend_bool pfree;

		keyvault_namespace = php_array_fetchc_string(options, "keyVaultNamespace", &plen, &pfree);

		if (!phongo_split_namespace(keyvault_namespace, &db_name, &coll_name)) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"keyVaultNamespace\" encryption option to contain a full collection name");

			if (pfree) {
				efree(keyvault_namespace);
			}

			goto cleanup;
		}

		mongoc_client_encryption_opts_set_keyvault_namespace(opts, db_name, coll_name);
		efree(db_name);
		efree(coll_name);

		if (pfree) {
			efree(keyvault_namespace);
		}
	}

	if (php_array_existsc(options, "kmsProviders")) {
		zval*  kms_providers  = php_array_fetchc(options, "kmsProviders");
		bson_t bson_providers = BSON_INITIALIZER;

		if (Z_TYPE_P(kms_providers) != IS_ARRAY && Z_TYPE_P(kms_providers) != IS_OBJECT) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected \"kmsProviders\" encryption option to be an array or object");
			goto cleanup;
		}

		php_phongo_zval_to_bson(kms_providers, PHONGO_BSON_NONE, &bson_providers, NULL);
		if (EG(exception)) {
			goto cleanup;
		}

		mongoc_client_encryption_opts_set_kms_providers(opts, &bson_providers);
		bson_destroy(&bson_providers);
	}

	return opts;

cleanup:
	if (opts) {
		mongoc_client_encryption_opts_destroy(opts);
	}

	return NULL;
} /* }}} */

void phongo_clientencryption_init(php_phongo_clientencryption_t* clientencryption, zval* manager, zval* options) /* {{{ */
{
	mongoc_client_encryption_t*      ce;
	mongoc_client_encryption_opts_t* opts;
	zval*                            key_vault_client_manager = manager;
	bson_error_t                     error                    = { 0 };

	opts = phongo_clientencryption_opts_from_zval(manager, options, &key_vault_client_manager);
	if (!opts) {
		/* Exception already thrown */
		goto cleanup;
	}

	ce = mongoc_client_encryption_new(opts, &error);
	if (!ce) {
		phongo_throw_exception_from_bson_error_t(&error);

		goto cleanup;
	}

	clientencryption->client_encryption = ce;
	ZVAL_ZVAL(&clientencryption->key_vault_client_manager, key_vault_client_manager, 1, 0);

cleanup:
	if (opts) {
		mongoc_client_encryption_opts_destroy(opts);
	}
} /* }}} */

static mongoc_client_encryption_datakey_opts_t* phongo_clientencryption_datakey_opts_from_zval(zval* options) /* {{{ */
{
	mongoc_client_encryption_datakey_opts_t* opts;

	opts = mongoc_client_encryption_datakey_opts_new();

	if (!options || Z_TYPE_P(options) != IS_ARRAY) {
		return opts;
	}

	if (php_array_existsc(options, "keyAltNames")) {
		zval*      zkeyaltnames = php_array_fetchc(options, "keyAltNames");
		HashTable* ht_data;
		uint32_t   keyaltnames_count;
		char**     keyaltnames;
		uint32_t   i      = 0;
		uint32_t   j      = 0;
		bool       failed = false;

		if (!zkeyaltnames || Z_TYPE_P(zkeyaltnames) != IS_ARRAY) {
			phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected keyAltNames to be array, %s given", PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(zkeyaltnames));
			goto cleanup;
		}

		ht_data           = HASH_OF(zkeyaltnames);
		keyaltnames_count = ht_data ? zend_hash_num_elements(ht_data) : 0;
		keyaltnames       = ecalloc(keyaltnames_count, sizeof(char*));

		{
			zend_string* string_key = NULL;
			zend_ulong   num_key    = 0;
			zval*        keyaltname;

			ZEND_HASH_FOREACH_KEY_VAL(ht_data, num_key, string_key, keyaltname)
			{
				if (i >= keyaltnames_count) {
					phongo_throw_exception(PHONGO_ERROR_LOGIC, "Iterating over too many keyAltNames. Please file a bug report");
					failed = true;
					break;
				}

				if (Z_TYPE_P(keyaltname) != IS_STRING) {
					if (string_key) {
						phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected keyAltName with index \"%s\" to be string, %s given", ZSTR_VAL(string_key), PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(keyaltname));
					} else {
						phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Expected keyAltName with index \"%lu\" to be string, %s given", num_key, PHONGO_ZVAL_CLASS_OR_TYPE_NAME_P(keyaltname));
					}

					failed = true;
					break;
				}

				keyaltnames[i] = estrdup(Z_STRVAL_P(keyaltname));
				i++;
			}
			ZEND_HASH_FOREACH_END();
		}

		if (!failed) {
			mongoc_client_encryption_datakey_opts_set_keyaltnames(opts, keyaltnames, keyaltnames_count);
		}

		for (j = 0; j < i; j++) {
			efree(keyaltnames[j]);
		}
		efree(keyaltnames);

		if (failed) {
			goto cleanup;
		}
	}

	if (php_array_existsc(options, "masterKey")) {
		bson_t masterkey = BSON_INITIALIZER;

		php_phongo_zval_to_bson(php_array_fetchc(options, "masterKey"), PHONGO_BSON_NONE, &masterkey, NULL);
		if (EG(exception)) {
			goto cleanup;
		}

		mongoc_client_encryption_datakey_opts_set_masterkey(opts, &masterkey);
	}

	return opts;

cleanup:
	if (opts) {
		mongoc_client_encryption_datakey_opts_destroy(opts);
	}

	return NULL;
} /* }}} */

void phongo_clientencryption_create_datakey(php_phongo_clientencryption_t* clientencryption, zval* return_value, char* kms_provider, zval* options) /* {{{ */
{
	mongoc_client_encryption_datakey_opts_t* opts;
	bson_value_t                             keyid;
	bson_error_t                             error = { 0 };

	opts = phongo_clientencryption_datakey_opts_from_zval(options);
	if (!opts) {
		/* Exception already thrown */
		goto cleanup;
	}

	if (!mongoc_client_encryption_create_datakey(clientencryption->client_encryption, kms_provider, opts, &keyid, &error)) {
		phongo_throw_exception_from_bson_error_t(&error);
		goto cleanup;
	}

	if (!php_phongo_bson_value_to_zval(&keyid, return_value)) {
		/* Exception already thrown */
		goto cleanup;
	}

cleanup:
	if (opts) {
		mongoc_client_encryption_datakey_opts_destroy(opts);
	}
} /* }}} */

static mongoc_client_encryption_encrypt_opts_t* phongo_clientencryption_encrypt_opts_from_zval(zval* options) /* {{{ */
{
	mongoc_client_encryption_encrypt_opts_t* opts;

	opts = mongoc_client_encryption_encrypt_opts_new();

	if (!options || Z_TYPE_P(options) != IS_ARRAY) {
		return opts;
	}

	if (php_array_existsc(options, "keyId")) {
		bson_value_t keyid;

		php_phongo_zval_to_bson_value(php_array_fetchc(options, "keyId"), PHONGO_BSON_NONE, &keyid);
		if (EG(exception)) {
			goto cleanup;
		}

		mongoc_client_encryption_encrypt_opts_set_keyid(opts, &keyid);
	}

	if (php_array_existsc(options, "keyAltName")) {
		char*     keyaltname;
		int       plen;
		zend_bool pfree;

		keyaltname = php_array_fetch_string(options, "keyAltName", &plen, &pfree);
		mongoc_client_encryption_encrypt_opts_set_keyaltname(opts, keyaltname);

		if (pfree) {
			efree(keyaltname);
		}
	}

	if (php_array_existsc(options, "algorithm")) {
		char*     algorithm;
		int       plen;
		zend_bool pfree;

		algorithm = php_array_fetch_string(options, "algorithm", &plen, &pfree);
		mongoc_client_encryption_encrypt_opts_set_algorithm(opts, algorithm);

		if (pfree) {
			efree(algorithm);
		}
	}

	return opts;

cleanup:
	if (opts) {
		mongoc_client_encryption_encrypt_opts_destroy(opts);
	}

	return NULL;
} /* }}} */

void phongo_clientencryption_encrypt(php_phongo_clientencryption_t* clientencryption, zval* zvalue, zval* zciphertext, zval* options) /* {{{ */
{
	mongoc_client_encryption_encrypt_opts_t* opts;
	bson_value_t                             ciphertext, value;
	bson_error_t                             error = { 0 };

	php_phongo_zval_to_bson_value(zvalue, PHONGO_BSON_NONE, &value);

	opts = phongo_clientencryption_encrypt_opts_from_zval(options);
	if (!opts) {
		/* Exception already thrown */
		goto cleanup;
	}

	if (!mongoc_client_encryption_encrypt(clientencryption->client_encryption, &value, opts, &ciphertext, &error)) {
		phongo_throw_exception_from_bson_error_t(&error);
		goto cleanup;
	}

	if (!php_phongo_bson_value_to_zval(&ciphertext, zciphertext)) {
		/* Exception already thrown */
		goto cleanup;
	}

cleanup:
	if (opts) {
		mongoc_client_encryption_encrypt_opts_destroy(opts);
	}
} /* }}} */

void phongo_clientencryption_decrypt(php_phongo_clientencryption_t* clientencryption, zval* zciphertext, zval* zvalue) /* {{{ */
{
	bson_value_t ciphertext, value;
	bson_error_t error = { 0 };

	php_phongo_zval_to_bson_value(zciphertext, PHONGO_BSON_NONE, &ciphertext);

	if (!mongoc_client_encryption_decrypt(clientencryption->client_encryption, &ciphertext, &value, &error)) {
		phongo_throw_exception_from_bson_error_t(&error);
		return;
	}

	if (!php_phongo_bson_value_to_zval(&value, zvalue)) {
		/* Exception already thrown */
		return;
	}
}
/* }}} */
#else /* MONGOC_ENABLE_CLIENT_SIDE_ENCRYPTION */
static void phongo_throw_exception_no_cse(php_phongo_error_domain_t domain, const char* message) /* {{{ */
{
	phongo_throw_exception(domain, "%s Please recompile with support for libmongocrypt using the with-mongodb-client-side-encryption configure switch.", message);
}
/* }}} */

static bool phongo_manager_set_auto_encryption_opts(php_phongo_manager_t* manager, zval* driverOptions) /* {{{ */
{
	if (!driverOptions || !php_array_existsc(driverOptions, "autoEncryption")) {
		return true;
	}

	phongo_throw_exception_no_cse(PHONGO_ERROR_INVALID_ARGUMENT, "Cannot enable automatic field-level encryption.");

	return false;
}
/* }}} */

void phongo_clientencryption_init(php_phongo_clientencryption_t* clientencryption, zval* manager, zval* options) /* {{{ */
{
	phongo_throw_exception_no_cse(PHONGO_ERROR_RUNTIME, "Cannot configure clientEncryption object.");
}
/* }}} */

void phongo_clientencryption_create_datakey(php_phongo_clientencryption_t* clientencryption, zval* return_value, char* kms_provider, zval* options) /* {{{ */
{
	phongo_throw_exception_no_cse(PHONGO_ERROR_RUNTIME, "Cannot create encryption key.");
}
/* }}} */

void phongo_clientencryption_encrypt(php_phongo_clientencryption_t* clientencryption, zval* zvalue, zval* zciphertext, zval* options) /* {{{ */
{
	phongo_throw_exception_no_cse(PHONGO_ERROR_RUNTIME, "Cannot encrypt value.");
}
/* }}} */

void phongo_clientencryption_decrypt(php_phongo_clientencryption_t* clientencryption, zval* zciphertext, zval* zvalue) /* {{{ */
{
	phongo_throw_exception_no_cse(PHONGO_ERROR_RUNTIME, "Cannot decrypt value.");
}
/* }}} */
#endif

void phongo_manager_init(php_phongo_manager_t* manager, const char* uri_string, zval* options, zval* driverOptions) /* {{{ */
{
	bson_t        bson_options = BSON_INITIALIZER;
	mongoc_uri_t* uri          = NULL;
#ifdef MONGOC_ENABLE_SSL
	mongoc_ssl_opt_t* ssl_opt = NULL;
#endif

	if (!(manager->client_hash = php_phongo_manager_make_client_hash(uri_string, options, driverOptions, &manager->client_hash_len))) {
		/* Exception should already have been thrown and there is nothing to free */
		return;
	}

	if (driverOptions && php_array_existsc(driverOptions, "disableClientPersistence")) {
		manager->use_persistent_client = !php_array_fetchc_bool(driverOptions, "disableClientPersistence");
	} else {
		manager->use_persistent_client = true;
	}

	if (manager->use_persistent_client && (manager->client = php_phongo_find_persistent_client(manager->client_hash, manager->client_hash_len))) {
		MONGOC_DEBUG("Found client for hash: %s", manager->client_hash);
		goto cleanup;
	}

	if (options) {
		php_phongo_zval_to_bson(options, PHONGO_BSON_NONE, &bson_options, NULL);
	}

	/* An exception may be thrown during BSON conversion */
	if (EG(exception)) {
		goto cleanup;
	}

	if (!(uri = php_phongo_make_uri(uri_string))) {
		/* Exception should already have been thrown */
		goto cleanup;
	}

	if (!php_phongo_apply_options_to_uri(uri, &bson_options) ||
		!php_phongo_apply_rc_options_to_uri(uri, &bson_options) ||
		!php_phongo_apply_rp_options_to_uri(uri, &bson_options) ||
		!php_phongo_apply_wc_options_to_uri(uri, &bson_options)) {
		/* Exception should already have been thrown */
		goto cleanup;
	}

#ifdef MONGOC_ENABLE_SSL
	if (!php_phongo_apply_driver_options_to_uri(uri, driverOptions)) {
		/* Exception should already have been thrown */
		goto cleanup;
	}

	ssl_opt = php_phongo_make_ssl_opt(uri, driverOptions);

	/* An exception may be thrown during SSL option creation */
	if (EG(exception)) {
		goto cleanup;
	}

	if (!php_phongo_uri_finalize_tls(uri)) {
		/* Exception should already have been thrown */
		goto cleanup;
	}
#else
	if (mongoc_uri_get_tls(uri)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT, "Cannot create SSL client. SSL is not enabled in this build.");
		goto cleanup;
	}
#endif

	manager->client = php_phongo_make_mongo_client(uri, driverOptions);
	mongoc_client_set_error_api(manager->client, MONGOC_ERROR_API_VERSION_2);

	if (!manager->client) {
		phongo_throw_exception(PHONGO_ERROR_RUNTIME, "Failed to create Manager from URI: '%s'", uri_string);
		goto cleanup;
	}

#ifdef MONGOC_ENABLE_SSL
	if (ssl_opt) {
		mongoc_client_set_ssl_opts(manager->client, ssl_opt);
	}
#endif

	if (!phongo_manager_set_auto_encryption_opts(manager, driverOptions)) {
		/* Exception should already have been thrown */
		goto cleanup;
	}

	php_phongo_set_monitoring_callbacks(manager->client);

	MONGOC_DEBUG("Created client with hash: %s", manager->client_hash);

	/* Register the newly created client in the appropriate registry (for either
	 * persistent or request-scoped clients). */
	if (!php_phongo_client_register(manager)) {
		phongo_throw_exception(PHONGO_ERROR_UNEXPECTED_VALUE, "Failed to add Manager client to internal registry");
		goto cleanup;
	}

cleanup:
	bson_destroy(&bson_options);

	if (uri) {
		mongoc_uri_destroy(uri);
	}

#ifdef MONGOC_ENABLE_SSL
	if (ssl_opt) {
		php_phongo_free_ssl_opt(ssl_opt);
	}
#endif
} /* }}} */

bool php_phongo_parse_int64(int64_t* retval, const char* data, size_t data_len) /* {{{ */
{
	int64_t value;
	char*   endptr = NULL;

	/* bson_ascii_strtoll() sets errno if conversion fails. If conversion
	 * succeeds, we still want to ensure that the entire string was parsed. */
	value = bson_ascii_strtoll(data, &endptr, 10);

	if (errno || (endptr && endptr != ((const char*) data + data_len))) {
		return false;
	}

	*retval = value;

	return true;
} /* }}} */

/* {{{ Memory allocation wrappers */
static void* php_phongo_malloc(size_t num_bytes) /* {{{ */
{
	return pemalloc(num_bytes, 1);
} /* }}} */

static void* php_phongo_calloc(size_t num_members, size_t num_bytes) /* {{{ */
{
	return pecalloc(num_members, num_bytes, 1);
} /* }}} */

static void* php_phongo_realloc(void* mem, size_t num_bytes) /* {{{ */
{
	return perealloc(mem, num_bytes, 1);
} /* }}} */

static void php_phongo_free(void* mem) /* {{{ */
{
	if (mem) {
		pefree(mem, 1);
	}
} /* }}} */

/* }}} */

/* {{{ M[INIT|SHUTDOWN] R[INIT|SHUTDOWN] G[INIT|SHUTDOWN] MINFO INI */

ZEND_INI_MH(OnUpdateDebug)
{
	void*** ctx     = NULL;
	char*   tmp_dir = NULL;

	/* Close any previously open log files */
	if (MONGODB_G(debug_fd)) {
		if (MONGODB_G(debug_fd) != stderr && MONGODB_G(debug_fd) != stdout) {
			fclose(MONGODB_G(debug_fd));
		}
		MONGODB_G(debug_fd) = NULL;
	}

	if (!new_value || (new_value && !ZSTR_VAL(new_value)[0]) || strcasecmp("0", ZSTR_VAL(new_value)) == 0 || strcasecmp("off", ZSTR_VAL(new_value)) == 0 || strcasecmp("no", ZSTR_VAL(new_value)) == 0 || strcasecmp("false", ZSTR_VAL(new_value)) == 0) {
		mongoc_log_trace_disable();
		mongoc_log_set_handler(NULL, NULL);

		return OnUpdateString(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);
	}

	if (strcasecmp(ZSTR_VAL(new_value), "stderr") == 0) {
		MONGODB_G(debug_fd) = stderr;
	} else if (strcasecmp(ZSTR_VAL(new_value), "stdout") == 0) {
		MONGODB_G(debug_fd) = stdout;
	} else if (
		strcasecmp("1", ZSTR_VAL(new_value)) == 0 ||
		strcasecmp("on", ZSTR_VAL(new_value)) == 0 ||
		strcasecmp("yes", ZSTR_VAL(new_value)) == 0 ||
		strcasecmp("true", ZSTR_VAL(new_value)) == 0) {

		tmp_dir = NULL;
	} else {
		tmp_dir = ZSTR_VAL(new_value);
	}

	if (!MONGODB_G(debug_fd)) {
		time_t       t;
		int          fd = -1;
		char*        prefix;
		int          len;
		zend_string* filename;

		time(&t);
		len = spprintf(&prefix, 0, "PHONGO-%ld", t);

		fd = php_open_temporary_fd(tmp_dir, prefix, &filename);
		if (fd != -1) {
			const char* path    = ZSTR_VAL(filename);
			MONGODB_G(debug_fd) = VCWD_FOPEN(path, "a");
		}
		efree(filename);
		efree(prefix);
		close(fd);
	}

	mongoc_log_trace_enable();
	mongoc_log_set_handler(php_phongo_log, ctx);

	return OnUpdateString(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);
}

/* {{{ INI entries */
PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY(PHONGO_DEBUG_INI, PHONGO_DEBUG_INI_DEFAULT, PHP_INI_ALL, OnUpdateDebug, debug, zend_mongodb_globals, mongodb_globals)
PHP_INI_END()
/* }}} */

static void phongo_pclient_reset_once(php_phongo_pclient_t* pclient, int pid)
{
	if (pclient->last_reset_by_pid != pid) {
		mongoc_client_reset(pclient->client);
		pclient->last_reset_by_pid = pid;
	}
}

/* Resets the libmongoc client if it has not already been reset for the current
 * PID (based on information in the hash table of persisted libmongoc clients).
 * This ensures that we do not reset a client multiple times from the same child
 * process. */
void php_phongo_client_reset_once(php_phongo_manager_t* manager, int pid)
{
	php_phongo_pclient_t* pclient;

	/* Reset associated key vault client */
	if (!Z_ISUNDEF(manager->key_vault_client_manager)) {
		php_phongo_client_reset_once(Z_MANAGER_OBJ_P(&manager->key_vault_client_manager), pid);
	}

	if (manager->use_persistent_client) {
		pclient = zend_hash_str_find_ptr(&MONGODB_G(persistent_clients), manager->client_hash, manager->client_hash_len);

		if (pclient) {
			phongo_pclient_reset_once(pclient, pid);
		}

		return;
	}

	ZEND_HASH_FOREACH_PTR(MONGODB_G(request_clients), pclient)
	{
		if (pclient->client == manager->client) {
			phongo_pclient_reset_once(pclient, pid);
			return;
		}
	}
	ZEND_HASH_FOREACH_END();
}

static void php_phongo_pclient_destroy(php_phongo_pclient_t* pclient)
{
	/* Do not destroy mongoc_client_t objects created by other processes. This
	 * ensures that we do not shutdown sockets that may still be in use by our
	 * parent process (see: PHPC-1522).
	 *
	 * This is a leak; however, we are already in GSHUTDOWN for a persistent
	 * clients. For a request-scoped client, we are either in the Manager's
	 * free_object handler or RSHUTDOWN, but there the application is capable of
	 * freeing its Manager and its client before forking. */
	if (pclient->created_by_pid == getpid()) {
		/* Single-threaded clients may run commands (e.g. endSessions) from
		 * mongoc_client_destroy, so disable APM to ensure an event is not
		 * dispatched while destroying the Manager and its client. This means
		 * that certain shutdown commands cannot be observed unless APM is
		 * redesigned to not reference a client (see: PHPC-1666).
		 *
		 * Note: this is only relevant for request-scoped clients. APM
		 * subscribers will no longer exist when persistent clients are
		 * destroyed in GSHUTDOWN. */
		mongoc_client_set_apm_callbacks(pclient->client, NULL, NULL);
		mongoc_client_destroy(pclient->client);
	}

	/* Persistent and request-scoped clients use different memory allocation */
	pefree(pclient, pclient->is_persistent);
}

/* Returns whether a Manager exists in the request-scoped registry. If found and
 * the output parameter is non-NULL, the Manager's index will be assigned. */
static bool php_phongo_manager_exists(php_phongo_manager_t* manager, zend_ulong* index_out)
{
	zend_ulong            index;
	php_phongo_manager_t* value;

	if (!MONGODB_G(managers) || zend_hash_num_elements(MONGODB_G(managers)) == 0) {
		return false;
	}

	ZEND_HASH_FOREACH_NUM_KEY_PTR(MONGODB_G(managers), index, value)
	{
		if (value != manager) {
			continue;
		}

		if (index_out) {
			*index_out = index;
		}

		return true;
	}
	ZEND_HASH_FOREACH_END();

	return false;
}

/* Adds a Manager to the request-scoped registry. Returns true if the Manager
 * did not exist and was successfully added; otherwise, returns false. */
bool php_phongo_manager_register(php_phongo_manager_t* manager)
{
	if (!MONGODB_G(managers)) {
		return false;
	}

	if (php_phongo_manager_exists(manager, NULL)) {
		return false;
	}

	return zend_hash_next_index_insert_ptr(MONGODB_G(managers), manager) != NULL;
}

/* Removes a Manager from the request-scoped registry. Returns true if the
 * Manager was found and successfully removed; otherwise, false is returned. */
bool php_phongo_manager_unregister(php_phongo_manager_t* manager)
{
	zend_ulong index;

	/* Ensure the registry is initialized. This is needed because RSHUTDOWN may
	 * occur before a Manager's free_object handler is executed. */
	if (!MONGODB_G(managers)) {
		return false;
	}

	if (php_phongo_manager_exists(manager, &index)) {
		return zend_hash_index_del(MONGODB_G(managers), index) == SUCCESS;
	}

	return false;
}

static void php_phongo_pclient_destroy_ptr(zval* ptr)
{
	php_phongo_pclient_destroy(Z_PTR_P(ptr));
}

/* {{{ PHP_RINIT_FUNCTION */
PHP_RINIT_FUNCTION(mongodb)
{
	/* Initialize HashTable for non-persistent clients, which is initialized to
	 * NULL in GINIT and destroyed and reset to NULL in RSHUTDOWN. Although we
	 * specify an element destructor here, all request clients should be freed
	 * naturally via garbage collection (i.e. the HashTable should be empty at
	 * the time it is destroyed in RSHUTDOWN). */
	if (MONGODB_G(request_clients) == NULL) {
		ALLOC_HASHTABLE(MONGODB_G(request_clients));
		zend_hash_init(MONGODB_G(request_clients), 0, NULL, php_phongo_pclient_destroy_ptr, 0);
	}

	/* Initialize HashTable for APM subscribers, which is initialized to NULL in
	 * GINIT and destroyed and reset to NULL in RSHUTDOWN. Since this HashTable
	 * will store subscriber object zvals, we specify ZVAL_PTR_DTOR as its
	 * element destructor so that any still-registered subscribers can be freed
	 * in RSHUTDOWN. */
	if (MONGODB_G(subscribers) == NULL) {
		ALLOC_HASHTABLE(MONGODB_G(subscribers));
		zend_hash_init(MONGODB_G(subscribers), 0, NULL, ZVAL_PTR_DTOR, 0);
	}

	/* Initialize HashTable for registering Manager objects. This is initialized
	 * to NULL in GINIT and destroyed and reset to NULL in RSHUTDOWN. Since this
	 * HashTable stores pointers to existing php_phongo_manager_t objects (not
	 * counted references), the element destructor is intentionally NULL. */
	if (MONGODB_G(managers) == NULL) {
		ALLOC_HASHTABLE(MONGODB_G(managers));
		zend_hash_init(MONGODB_G(managers), 0, NULL, NULL, 0);
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_GINIT_FUNCTION */
PHP_GINIT_FUNCTION(mongodb)
{
#if defined(COMPILE_DL_MONGODB) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	/* Increment the thread counter. */
	bson_atomic_int_add(&phongo_num_threads, 1);

	/* Clear extension globals */
	memset(mongodb_globals, 0, sizeof(zend_mongodb_globals));

	/* Initialize HashTable for persistent clients, which will be destroyed in
	 * GSHUTDOWN. We specify an element destructor so that persistent clients
	 * can be destroyed along with the HashTable. The HashTable's struct is
	 * nested within globals, so no allocation is needed (unlike the HashTables
	 * allocated in RINIT). */
	zend_hash_init(&mongodb_globals->persistent_clients, 0, NULL, php_phongo_pclient_destroy_ptr, 1);
}
/* }}} */

static zend_class_entry* php_phongo_fetch_internal_class(const char* class_name, size_t class_name_len)
{
	zend_class_entry* pce;

	if ((pce = zend_hash_str_find_ptr(CG(class_table), class_name, class_name_len))) {
		return pce;
	}

	return NULL;
}

static HashTable* php_phongo_std_get_gc(phongo_compat_object_handler_type* object, zval** table, int* n) /* {{{ */
{
	*table = NULL;
	*n     = 0;
	return zend_std_get_properties(object);
} /* }}} */

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(mongodb)
{
	bson_mem_vtable_t bson_mem_vtable = {
		php_phongo_malloc,
		php_phongo_calloc,
		php_phongo_realloc,
		php_phongo_free,
	};

	(void) type; /* We don't care if we are loaded via dl() or extension= */

	REGISTER_INI_ENTRIES();

	/* Assign our custom vtable to libbson, so all memory allocation in libbson
	 * (and libmongoc) will use PHP's persistent memory API. After doing so,
	 * initialize libmongoc. Later, we will shutdown libmongoc and restore
	 * libbson's vtable in the final GSHUTDOWN. */
	bson_mem_set_vtable(&bson_mem_vtable);
	mongoc_init();

	/* Prep default object handlers to be used when we register the classes */
	memcpy(&phongo_std_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	/* Disable cloning by default. Individual classes can opt in if they need to
	 * support this (e.g. BSON objects). */
	phongo_std_object_handlers.clone_obj = NULL;
	/* Ensure that get_gc delegates to zend_std_get_properties directly in case
	 * our class defines a get_properties handler for debugging purposes. */
	phongo_std_object_handlers.get_gc = php_phongo_std_get_gc;

	/* Initialize zend_class_entry dependencies.
	 *
	 * Although DateTimeImmutable was introduced in PHP 5.5.0,
	 * php_date_get_immutable_ce() is not available in PHP versions before
	 * 5.5.24 and 5.6.8.
	 *
	 * Although JsonSerializable was introduced in PHP 5.4.0,
	 * php_json_serializable_ce is not exported in PHP versions before 5.4.26
	 * and 5.5.10. For later PHP versions, looking up the class manually also
	 * helps with distros that disable LTDL_LAZY for dlopen() (e.g. Fedora).
	 */
	php_phongo_date_immutable_ce    = php_phongo_fetch_internal_class(ZEND_STRL("datetimeimmutable"));
	php_phongo_json_serializable_ce = php_phongo_fetch_internal_class(ZEND_STRL("jsonserializable"));

	if (php_phongo_json_serializable_ce == NULL) {
		zend_error(E_ERROR, "JsonSerializable class is not defined. Please ensure that the 'json' module is loaded before the 'mongodb' module.");
		return FAILURE;
	}

	/* Register base BSON classes first */
	php_phongo_type_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_serializable_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_unserializable_init_ce(INIT_FUNC_ARGS_PASSTHRU);

	php_phongo_binary_interface_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_decimal128_interface_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_javascript_interface_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_maxkey_interface_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_minkey_interface_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_objectid_interface_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_regex_interface_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_timestamp_interface_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_utcdatetime_interface_init_ce(INIT_FUNC_ARGS_PASSTHRU);

	php_phongo_binary_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_dbpointer_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_decimal128_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_int64_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_javascript_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_maxkey_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_minkey_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_objectid_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_persistable_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_regex_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_symbol_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_timestamp_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_undefined_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_utcdatetime_init_ce(INIT_FUNC_ARGS_PASSTHRU);

	php_phongo_cursor_interface_init_ce(INIT_FUNC_ARGS_PASSTHRU);

	php_phongo_bulkwrite_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_clientencryption_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_command_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_cursor_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_cursorid_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_manager_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_query_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_readconcern_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_readpreference_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_server_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_session_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_writeconcern_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_writeconcernerror_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_writeerror_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_writeresult_init_ce(INIT_FUNC_ARGS_PASSTHRU);

	/* Register base exception classes first */
	php_phongo_exception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_runtimeexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_serverexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_connectionexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_writeexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);

	php_phongo_authenticationexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_bulkwriteexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_commandexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_connectiontimeoutexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_encryptionexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_executiontimeoutexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_invalidargumentexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_logicexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_sslconnectionexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_unexpectedvalueexception_init_ce(INIT_FUNC_ARGS_PASSTHRU);

	/* Register base APM classes first */
	php_phongo_subscriber_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_commandsubscriber_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_commandfailedevent_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_commandstartedevent_init_ce(INIT_FUNC_ARGS_PASSTHRU);
	php_phongo_commandsucceededevent_init_ce(INIT_FUNC_ARGS_PASSTHRU);

	REGISTER_STRING_CONSTANT("MONGODB_VERSION", (char*) PHP_MONGODB_VERSION, CONST_CS | CONST_PERSISTENT);
	REGISTER_STRING_CONSTANT("MONGODB_STABILITY", (char*) PHP_MONGODB_STABILITY, CONST_CS | CONST_PERSISTENT);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(mongodb)
{
	UNREGISTER_INI_ENTRIES();

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION */
PHP_RSHUTDOWN_FUNCTION(mongodb)
{
	/* Destroy HashTable for APM subscribers, which was initialized in RINIT. */
	if (MONGODB_G(subscribers)) {
		zend_hash_destroy(MONGODB_G(subscribers));
		FREE_HASHTABLE(MONGODB_G(subscribers));
		MONGODB_G(subscribers) = NULL;
	}

	/* Destroy HashTable for non-persistent clients, which was initialized in
	 * RINIT. This is intentionally done after the APM subscribers to allow any
	 * non-persistent clients still referenced by a subscriber (not freed prior
	 * to RSHUTDOWN) to be naturally garbage collected and freed by the Manager
	 * free_object handler rather than the HashTable's element destructor. There
	 * is no need to use zend_hash_graceful_reverse_destroy here like we do for
	 * persistent clients; moreover, the HashTable should already be empty. */
	if (MONGODB_G(request_clients)) {
		zend_hash_destroy(MONGODB_G(request_clients));
		FREE_HASHTABLE(MONGODB_G(request_clients));
		MONGODB_G(request_clients) = NULL;
	}

	/* Destroy HashTable for Managers, which was initialized in RINIT. */
	if (MONGODB_G(managers)) {
		zend_hash_destroy(MONGODB_G(managers));
		FREE_HASHTABLE(MONGODB_G(managers));
		MONGODB_G(managers) = NULL;
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_GSHUTDOWN_FUNCTION */
PHP_GSHUTDOWN_FUNCTION(mongodb)
{
	/* Destroy persistent client HashTable in reverse order. This is necessary
	 * to prevent segmentation faults as clients may reference other clients in
	 * encryption settings. */
	zend_hash_graceful_reverse_destroy(&mongodb_globals->persistent_clients);

	mongodb_globals->debug = NULL;
	if (mongodb_globals->debug_fd) {
		fclose(mongodb_globals->debug_fd);
		mongodb_globals->debug_fd = NULL;
	}

	/* Decrement the thread counter. If it reaches zero, we can infer that this
	 * is the last thread, MSHUTDOWN has been called, persistent clients from
	 * all threads have been destroyed, and it is now safe to shutdown libmongoc
	 * and restore libbson's original vtable. */
	if (bson_atomic_int_add(&phongo_num_threads, -1) == 0) {
		mongoc_cleanup();
		bson_mem_restore_vtable();
	}
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(mongodb)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "MongoDB support", "enabled");
	php_info_print_table_row(2, "MongoDB extension version", PHP_MONGODB_VERSION);
	php_info_print_table_row(2, "MongoDB extension stability", PHP_MONGODB_STABILITY);

#ifdef HAVE_SYSTEM_LIBBSON
	php_info_print_table_row(2, "libbson headers version", BSON_VERSION_S);
	php_info_print_table_row(2, "libbson library version", bson_get_version());
#else
	php_info_print_table_row(2, "libbson bundled version", BSON_VERSION_S);
#endif

#ifdef HAVE_SYSTEM_LIBMONGOC
	php_info_print_table_row(2, "libmongoc headers version", MONGOC_VERSION_S);
	php_info_print_table_row(2, "libmongoc library version", mongoc_get_version());
#else
	/* Bundled libraries, buildtime = runtime */
	php_info_print_table_row(2, "libmongoc bundled version", MONGOC_VERSION_S);
#endif

#ifdef MONGOC_ENABLE_SSL
	php_info_print_table_row(2, "libmongoc SSL", "enabled");
#if defined(MONGOC_ENABLE_SSL_OPENSSL)
	php_info_print_table_row(2, "libmongoc SSL library", "OpenSSL");
#elif defined(MONGOC_ENABLE_SSL_LIBRESSL)
	php_info_print_table_row(2, "libmongoc SSL library", "LibreSSL");
#elif defined(MONGOC_ENABLE_SSL_SECURE_TRANSPORT)
	php_info_print_table_row(2, "libmongoc SSL library", "Secure Transport");
#elif defined(MONGOC_ENABLE_SSL_SECURE_CHANNEL)
	php_info_print_table_row(2, "libmongoc SSL library", "Secure Channel");
#else
	php_info_print_table_row(2, "libmongoc SSL library", "unknown");
#endif
#else /* MONGOC_ENABLE_SSL */
	php_info_print_table_row(2, "libmongoc SSL", "disabled");
#endif

#ifdef MONGOC_ENABLE_CRYPTO
	php_info_print_table_row(2, "libmongoc crypto", "enabled");
#if defined(MONGOC_ENABLE_CRYPTO_LIBCRYPTO)
	php_info_print_table_row(2, "libmongoc crypto library", "libcrypto");
#elif defined(MONGOC_ENABLE_CRYPTO_COMMON_CRYPTO)
	php_info_print_table_row(2, "libmongoc crypto library", "Common Crypto");
#elif defined(MONGOC_ENABLE_CRYPTO_CNG)
	php_info_print_table_row(2, "libmongoc crypto library", "CNG");
#else
	php_info_print_table_row(2, "libmongoc crypto library", "unknown");
#endif
#ifdef MONGOC_ENABLE_CRYPTO_SYSTEM_PROFILE
	php_info_print_table_row(2, "libmongoc crypto system profile", "enabled");
#else
	php_info_print_table_row(2, "libmongoc crypto system profile", "disabled");
#endif
#else /* MONGOC_ENABLE_CRYPTO */
	php_info_print_table_row(2, "libmongoc crypto", "disabled");
#endif

#ifdef MONGOC_ENABLE_SASL
	php_info_print_table_row(2, "libmongoc SASL", "enabled");
#else
	php_info_print_table_row(2, "libmongoc SASL", "disabled");
#endif

#ifdef MONGOC_ENABLE_ICU
	php_info_print_table_row(2, "libmongoc ICU", "enabled");
#else
	php_info_print_table_row(2, "libmongoc ICU", "disabled");
#endif

#ifdef MONGOC_ENABLE_COMPRESSION
	php_info_print_table_row(2, "libmongoc compression", "enabled");
#ifdef MONGOC_ENABLE_COMPRESSION_SNAPPY
	php_info_print_table_row(2, "libmongoc compression snappy", "enabled");
#else
	php_info_print_table_row(2, "libmongoc compression snappy", "disabled");
#endif
#ifdef MONGOC_ENABLE_COMPRESSION_ZLIB
	php_info_print_table_row(2, "libmongoc compression zlib", "enabled");
#else
	php_info_print_table_row(2, "libmongoc compression zlib", "disabled");
#endif
#ifdef MONGOC_ENABLE_COMPRESSION_ZSTD
	php_info_print_table_row(2, "libmongoc compression zstd", "enabled");
#else
	php_info_print_table_row(2, "libmongoc compression zstd", "disabled");
#endif
#else /* MONGOC_ENABLE_COMPRESSION */
	php_info_print_table_row(2, "libmongoc compression", "disabled");
#endif

#ifdef MONGOC_ENABLE_CLIENT_SIDE_ENCRYPTION
#ifdef HAVE_SYSTEM_LIBMONGOCRYPT
	php_info_print_table_row(2, "libmongocrypt headers version", MONGOCRYPT_VERSION);
	php_info_print_table_row(2, "libmongocrypt library version", mongocrypt_version(NULL));
#else
	php_info_print_table_row(2, "libmongocrypt bundled version", MONGOCRYPT_VERSION);
#endif

#ifdef MONGOCRYPT_ENABLE_CRYPTO
	php_info_print_table_row(2, "libmongocrypt crypto", "enabled");

#if defined(MONGOCRYPT_ENABLE_CRYPTO_LIBCRYPTO)
	php_info_print_table_row(2, "libmongocrypt crypto library", "libcrypto");
#elif defined(MONGOCRYPT_ENABLE_CRYPTO_COMMON_CRYPTO)
	php_info_print_table_row(2, "libmongocrypt crypto library", "Common Crypto");
#elif defined(MONGOCRYPT_ENABLE_CRYPTO_CNG)
	php_info_print_table_row(2, "libmongocrypt crypto library", "CNG");
#else
	php_info_print_table_row(2, "libmongocrypt crypto library", "unknown");
#endif
#else /* MONGOCRYPT_ENABLE_CRYPTO */
	php_info_print_table_row(2, "libmongocrypt crypto", "disabled");
#endif
#else /* MONGOC_ENABLE_CLIENT_SIDE_ENCRYPTION */
	php_info_print_table_row(2, "libmongocrypt", "disabled");
#endif

	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */
/* }}} */

/* {{{ Shared function entries for disabling constructors and unserialize() */
PHP_FUNCTION(MongoDB_disabled___construct) /* {{{ */
{
	phongo_throw_exception(PHONGO_ERROR_RUNTIME, "Accessing private constructor");
} /* }}} */

PHP_FUNCTION(MongoDB_disabled___wakeup) /* {{{ */
{
	zend_error_handling error_handling;

	zend_replace_error_handling(EH_THROW, phongo_exception_from_phongo_domain(PHONGO_ERROR_INVALID_ARGUMENT), &error_handling);
	if (zend_parse_parameters_none() == FAILURE) {
		zend_restore_error_handling(&error_handling);
		return;
	}
	zend_restore_error_handling(&error_handling);

	phongo_throw_exception(PHONGO_ERROR_RUNTIME, "%s", "MongoDB\\Driver objects cannot be serialized");
} /* }}} */
  /* }}} */

/* {{{ mongodb_functions[]
*/
ZEND_BEGIN_ARG_INFO_EX(ai_bson_fromPHP, 0, 0, 1)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO();

ZEND_BEGIN_ARG_INFO_EX(ai_bson_toPHP, 0, 0, 1)
	ZEND_ARG_INFO(0, bson)
	ZEND_ARG_ARRAY_INFO(0, typemap, 0)
ZEND_END_ARG_INFO();

ZEND_BEGIN_ARG_INFO_EX(ai_bson_toJSON, 0, 0, 1)
	ZEND_ARG_INFO(0, bson)
ZEND_END_ARG_INFO();

ZEND_BEGIN_ARG_INFO_EX(ai_bson_fromJSON, 0, 0, 1)
	ZEND_ARG_INFO(0, json)
ZEND_END_ARG_INFO();

ZEND_BEGIN_ARG_INFO_EX(ai_mongodb_driver_monitoring_subscriber, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, subscriber, MongoDB\\Driver\\Monitoring\\Subscriber, 0)
ZEND_END_ARG_INFO();

static const zend_function_entry mongodb_functions[] = {
	ZEND_NS_NAMED_FE("MongoDB\\BSON", fromPHP, PHP_FN(MongoDB_BSON_fromPHP), ai_bson_fromPHP)
		ZEND_NS_NAMED_FE("MongoDB\\BSON", toPHP, PHP_FN(MongoDB_BSON_toPHP), ai_bson_toPHP)
			ZEND_NS_NAMED_FE("MongoDB\\BSON", toJSON, PHP_FN(MongoDB_BSON_toJSON), ai_bson_toJSON)
				ZEND_NS_NAMED_FE("MongoDB\\BSON", toCanonicalExtendedJSON, PHP_FN(MongoDB_BSON_toCanonicalExtendedJSON), ai_bson_toJSON)
					ZEND_NS_NAMED_FE("MongoDB\\BSON", toRelaxedExtendedJSON, PHP_FN(MongoDB_BSON_toRelaxedExtendedJSON), ai_bson_toJSON)
						ZEND_NS_NAMED_FE("MongoDB\\BSON", fromJSON, PHP_FN(MongoDB_BSON_fromJSON), ai_bson_fromJSON)
							ZEND_NS_NAMED_FE("MongoDB\\Driver\\Monitoring", addSubscriber, PHP_FN(MongoDB_Driver_Monitoring_addSubscriber), ai_mongodb_driver_monitoring_subscriber)
								ZEND_NS_NAMED_FE("MongoDB\\Driver\\Monitoring", removeSubscriber, PHP_FN(MongoDB_Driver_Monitoring_removeSubscriber), ai_mongodb_driver_monitoring_subscriber)
									PHP_FE_END
};
/* }}} */

static const zend_module_dep mongodb_deps[] = {
	ZEND_MOD_REQUIRED("date")
		ZEND_MOD_REQUIRED("json")
			ZEND_MOD_REQUIRED("spl")
				ZEND_MOD_REQUIRED("standard")
					ZEND_MOD_END
};

/* {{{ mongodb_module_entry
 */
zend_module_entry mongodb_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	mongodb_deps,
	"mongodb",
	mongodb_functions,
	PHP_MINIT(mongodb),
	PHP_MSHUTDOWN(mongodb),
	PHP_RINIT(mongodb),
	PHP_RSHUTDOWN(mongodb),
	PHP_MINFO(mongodb),
	PHP_MONGODB_VERSION,
	PHP_MODULE_GLOBALS(mongodb),
	PHP_GINIT(mongodb),
	PHP_GSHUTDOWN(mongodb),
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_MONGODB
ZEND_GET_MODULE(mongodb)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
