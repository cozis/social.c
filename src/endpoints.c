#include <stddef.h>
#include <stdint.h>
#include <stdlib.h> // exit
#include <assert.h>
#include <stdbool.h>
#include "log.h"
#include "basic.h"
#include "template.h"
#include "endpoints.h"
#include "sqlite_utils.h"

typedef uint32_t SessionID;
#define NO_SESSION ((SessionID) -1)

#define MAX_SESSIONS 512
#define MAX_USER_NAME 32
#define MAX_USER_PASS 256
#define MAX_USER_BIO  1024
#define MAX_POST_TITLE 1024
#define MAX_POST_CONTENT (1<<14)
#define MAX_COMMENT_CONTENT (1<<12)

typedef struct {
	uint32_t id;
	string name;
	char namebuf[MAX_USER_NAME];
} Session;

SessionID create_session(string name);
void      remove_session(SessionID id);
string    name_from_session(SessionID id);
SessionID session_from_request(Request request);

sqlite3 *db;
	
char schema[] =
	"CREATE TABLE IF NOT EXISTS Users(\n"
	"    name TEXT PRIMARY KEY,\n"
	"    pass TEXT NOT NULL,\n"
	"    bio  TEXT\n"
	");\n"
	"CREATE TABLE IF NOT EXISTS Posts(\n"
	"    id      INTEGER PRIMARY KEY,\n"
	"    title   TEXT NOT NULL,\n"
	"    content TEXT NOT NULL,\n"
	"    author  TEXT,\n"
	"    FOREIGN KEY (author) REFERENCES Users(name)\n"
	");\n"
	"CREATE TABLE IF NOT EXISTS Comments(\n"
	"    id      INTEGER PRIMARY KEY,\n"
	"    content TEXT NOT NULL,\n"
	"    author  TEXT,\n"
	"    parent  INTEGER,\n"
	"    FOREIGN KEY (author) REFERENCES Users(name),\n"
	"    FOREIGN KEY (parent) REFERENCES Posts(id)\n"
	");\n"
	"PRAGMA foreign_keys = ON;\n";

Session sessions[MAX_SESSIONS];
SessionID next_session_id = 1;

void init_endpoints(void)
{
	int code = sqlite3_open("file.db", &db);
	if (code != SQLITE_OK) {
		log_fatal(LIT("Couldn't open the database\n"));
		sqlite3_close(db);
		return;
	}

	{
		char *errmsg;
		int code = sqlite3_exec(db, schema, NULL, NULL, &errmsg);
		if (code != SQLITE_OK) {
			log_format("Couldn't apply database schema (%s)\n", errmsg);
			sqlite3_free(errmsg);
			sqlite3_close(db);
			exit(-1);
			return;
		}
	}

	for (int i = 0; i < MAX_SESSIONS; i++)
		sessions[i].id = NO_SESSION;
}

void free_endpoints(void)
{
	sqlite3_close(db);
}

void respond(Request request, ResponseBuilder *b)
{
	if (request.major != 1 || request.minor > 1) {
		status_line(b, 505); // HTTP Version Not Supported
		return;
	}

	SessionID sessid = session_from_request(request);
	string login_username = (sessid == NO_SESSION ? NULLSTR : name_from_session(sessid));

	if (streq(request.url.path, LIT("/")))
		request.url.path = LIT("/posts");

	if (streq(request.url.path, LIT("/action/login"))) {
		if (login_username.size > 0) {
			status_line(b, 303);
			add_header(b, LIT("Location: /"));
			return;
		}
		char namebuf[MAX_USER_NAME];
		char passbuf[MAX_USER_PASS];
		string name;		string pass;
		if (!get_query_string_param(request.content, LIT("name"), LIT(namebuf), &name)) {
			status_line(b, 500);
			append_content_s(b, LIT("Invalid name"));
			return;
		}
		if (!get_query_string_param(request.content, LIT("pass"), LIT(passbuf), &pass)) {
			status_line(b, 500);
			append_content_s(b, LIT("Invalid pass"));
			return;
		}
		int res = sqlite3_utils_rows_exist(db, "SELECT name FROM Users WHERE name=:s AND pass=:s", name, pass);
		if (res == -1) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			return;
		}
		if (res == 1) {
			// No such user
			status_line(b, 400);
			append_content_s(b, LIT("Invalid credentials"));
			return;
		}
		SessionID sessid = create_session(name);
		if (sessid == NO_SESSION) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			return;
		}
		// User exist
		status_line(b, 303);
		add_header_f(b, "Set-Cookie: sessid=%d; Path=/", sessid);
		add_header(b, LIT("Location: /"));
		return;
	}

	if (streq(request.url.path, LIT("/action/signup"))) {
		if (login_username.size > 0) {
			status_line(b, 303);
			add_header(b, LIT("Location: /"));
			return;
		}
		char namebuf[MAX_USER_NAME];
		char passbuf[MAX_USER_PASS];
		char  biobuf[MAX_USER_BIO];
		string name;
		string pass;
		string bio;
		if (!get_query_string_param(request.content, LIT("name"), LIT(namebuf), &name)) {
			status_line(b, 400);
			append_content_s(b, LIT("Invalid name"));
			return;
		}
		if (!get_query_string_param(request.content, LIT("pass"), LIT(passbuf), &pass)) {
			status_line(b, 400);
			append_content_s(b, LIT("Invalid pass"));
			return;
		}
		if (!get_query_string_param(request.content, LIT("bio"), LIT(biobuf), &bio)) {
			status_line(b, 400);
			append_content_s(b, LIT("Invalid bio"));
			return;
		}
		name = trim(name);
		pass = trim(pass);
		bio = trim(bio);
		if (name.size == 0 || pass.size == 0 || pass.size == 0) {
			status_line(b, 400);
			append_content_s(b, LIT("One or more fields are empty"));
			return;
		}
		if (!sqlite3_utils_exec(db, "INSERT INTO Users(name, pass, bio) VALUES (:s, :s, :s)", name, pass, bio)) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			return;
		}
		SessionID sessid = create_session(name);
		if (sessid == NO_SESSION) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			return;
		}
		status_line(b, 303);
		add_header_f(b, "Set-Cookie: sessid=%d; Path=/", sessid);
		add_header(b, LIT("Location: /"));
		return;
	}

	if (streq(request.url.path, LIT("/action/logout"))) {
		if (login_username.size > 0)
			remove_session(sessid);
		status_line(b, 303);
		add_header(b, LIT("Location: /login"));
		return;
	}

	if (streq(request.url.path, LIT("/action/post"))) {
		if (login_username.size == 0) {
			status_line(b, 400);
			append_content_s(b, LIT("Not logged in"));
			return;
		}
		char titlebuf[MAX_POST_TITLE];
		char contentbuf[MAX_POST_CONTENT];
		string title;
		string content;
		if (!get_query_string_param(request.content, LIT("title"), LIT(titlebuf), &title)) {
			status_line(b, 400);
			append_content_s(b, LIT("Invalid title"));
			return;
		}
		if (!get_query_string_param(request.content, LIT("content"), LIT(contentbuf), &content)) {
			status_line(b, 400);
			append_content_s(b, LIT("Invalid content"));
			return;
		}
		title = trim(title);
		content = trim(content);
		if (title.size == 0 || content.size == 0) {
			status_line(b, 400);
			append_content_s(b, LIT("One or more fields are empty"));
			return;
		}
		if (!sqlite3_utils_exec(db, "INSERT INTO Posts(title, content, author) VALUES (:s, :s, :s)", title, content, login_username)) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			return;
		}
		int64_t post_id = sqlite3_last_insert_rowid(db);
		status_line(b, 303);
		add_header_f(b, "Location: /posts/%d", post_id);
		return;
	}

	if (!match_path_format(request.url.path, "/posts")) {
		sqlite3_stmt *stmt = sqlite3_utils_prepare(db, "SELECT id, title, author FROM Posts");
		if (stmt == NULL) {
			status_line(b, 500);
			return;
		}
		status_line(b, 200);
		add_header(b, LIT("Content-Type: text/html"));
		TemplateParam params[] = {
			{.name=LIT("login"), .type=TPT_INT, .i=login_username.size>0},
			{.name=LIT("login_username"), .type=TPT_STRING, .s=login_username},
			{.name=LIT("posts"), .type=TPT_QUERY, .q=stmt},
			{.name=NULLSTR, .type=TPT_LAST }
		};
		append_template(b, LIT("pages/posts.html"), params);
		sqlite3_finalize(stmt);
		return;
	}

	int post_id;
	if (!match_path_format(request.url.path, "/posts/:n", &post_id)) {
		sqlite3_stmt *stmt = sqlite3_utils_prepare(db, "SELECT title, content, author FROM Posts WHERE id=:i", post_id);
		if (stmt == NULL) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			return;
		}
		sqlite3_stmt *stmt2 = sqlite3_utils_prepare(db, "SELECT id, content, author FROM Comments WHERE parent=:i", post_id);
		if (stmt2 == NULL) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			sqlite3_finalize(stmt);
			return;
		}
		string title;
		string content;
		string author;
		int res = sqlite3_utils_fetch(stmt, "sss", &title, &content, &author);
		if (res == -1) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			sqlite3_finalize(stmt);
			sqlite3_finalize(stmt2);
			return;
		}
		if (res == 1) {
			status_line(b, 404);
			append_content_s(b, LIT("No such post"));
			sqlite3_finalize(stmt);
			sqlite3_finalize(stmt2);
			return;
		}
		assert(res == 0);
		status_line(b, 200);
		add_header(b, LIT("Content-Type: text/html"));
		TemplateParam params[] = {
			{.name=LIT("login"),          .type=TPT_INT,    .i=login_username.size>0},
			{.name=LIT("login_username"), .type=TPT_STRING, .s=login_username},
			{.name=LIT("title"),          .type=TPT_STRING, .s=title},
			{.name=LIT("author"),         .type=TPT_STRING, .s=author},
			{.name=LIT("content"),        .type=TPT_STRING, .s=content},
			{.name=LIT("comments"),       .type=TPT_QUERY,  .q=stmt2},
			{.name=NULLSTR, .type=TPT_LAST }
		};
		append_template(b, LIT("pages/post.html"), params);
		sqlite3_finalize(stmt);
		sqlite3_finalize(stmt2);
		return;
	}

	int comment_post_id;
	if (!match_path_format(request.url.path, "/posts/:n/comments", &comment_post_id)) {
		if (login_username.size > 0) {
			status_line(b, 303);
			add_header(b, LIT("Location: /"));
			return;
		}
		char contentbuf[MAX_COMMENT_CONTENT];
		string content;
		if (!get_query_string_param(request.content, LIT("content"), LIT(contentbuf), &content)) {
			status_line(b, 400);
			append_content_s(b, LIT("Invalid content"));
			return;
		}
		content = trim(content);
		if (content.size == 0) {
			status_line(b, 400);
			append_content_s(b, LIT("Content field is empty"));
			return;
		}
		if (!sqlite3_utils_exec(db, "INSERT INTO Comments(parent, content, author) VALUES (:i, :s, :s)", comment_post_id, content, login_username)) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			return;
		}
		status_line(b, 303);
		add_header_f(b, "Location: /posts/%d", comment_post_id);
		return;
	}

	if (!match_path_format(request.url.path, "/users")) {
		sqlite3_stmt *stmt = sqlite3_utils_prepare(db, "SELECT name FROM Users");
		if (stmt == NULL) {
			status_line(b, 500);
			return;
		}
		status_line(b, 200);
		add_header(b, LIT("Content-Type: text/html"));
		TemplateParam params[] = {
			{.name=LIT("login"),          .type=TPT_INT,    .i=login_username.size>0},
			{.name=LIT("login_username"), .type=TPT_STRING, .s=login_username},
			{.name=LIT("users"),          .type=TPT_QUERY,  .q=stmt},
			{.name=NULLSTR, .type=TPT_LAST }
		};
		append_template(b, LIT("pages/users.html"), params);
		sqlite3_finalize(stmt);
		return;
	}

	string profile_username;
	if (!match_path_format(request.url.path, "/users/:s", &profile_username)) {
		sqlite3_stmt *stmt = sqlite3_utils_prepare(db, "SELECT bio FROM Users WHERE name=:s", profile_username);
		if (stmt == NULL) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			return;
		}
		string bio;
		int res = sqlite3_utils_fetch(stmt, "s", &bio);
		if (res == -1) {
			status_line(b, 500);
			append_content_s(b, LIT("Internal error"));
			return;
		}
		if (res == 1) {
			status_line(b, 404);
			append_content_s(b, LIT("No such user"));
			return;
		}
		assert(res == 0);
		status_line(b, 200);
		add_header(b, LIT("Content-Type: text/html"));
		TemplateParam params[] = {
			{.name=LIT("login"), .type=TPT_INT, .i=login_username.size>0},
			{.name=LIT("login_username"), .type=TPT_STRING, .s=login_username},
			{.name=LIT("name"), .type=TPT_STRING, .s=profile_username},
			{.name=LIT("bio"), .type=TPT_STRING, .s=bio},
			{.name=NULLSTR, .type=TPT_LAST }
		};
		append_template(b, LIT("pages/user.html"), params);
		sqlite3_finalize(stmt);
		return;
	}

	if (!match_path_format(request.url.path, "/login")) {
		if (login_username.size > 0) {
			// Already logged in
			status_line(b, 303);
			add_header(b, LIT("Location: /home"));
			return;
		}
		status_line(b, 200);
		append_file(b, LIT("pages/login.html"));
		return;
	}

	if (!match_path_format(request.url.path, "/signup")) {
		if (login_username.size > 0) {
			// Already logged in
			status_line(b, 303);
			add_header(b, LIT("Location: /home"));
			return;
		}
		status_line(b, 200);
		append_file(b, LIT("pages/signup.html"));
		return;
	}

	if (!match_path_format(request.url.path, "/home")) {
		status_line(b, 200);
		append_file(b, LIT("pages/home.html"));
		return;
	}

	if (serve_file_or_dir(b, LIT("/static"), LIT("static/"), request.url.path, NULLSTR, false))
		return;

	status_line(b, 404);
	append_content_s(b, LIT("Nothing here :|"));
}

///////////////////////////////////////////////////////////////////////////////////////////////
/// SESSIONS                                                                                ///
///////////////////////////////////////////////////////////////////////////////////////////////

SessionID create_session(string name)
{
	int i = 0;
	while (i < MAX_SESSIONS && sessions[i].id != NO_SESSION)
		i++;
	if (i == MAX_SESSIONS)
		return NO_SESSION;

	if (next_session_id == NO_SESSION)
		next_session_id++;
	SessionID id = next_session_id++;

	if (name.size > sizeof(sessions[i].namebuf))
		log_fatal(LIT("User name buffer is too small"));
	memcpy(sessions[i].namebuf, name.data, name.size);

	sessions[i].id = id;
	sessions[i].name = (string) { sessions[i].namebuf, name.size };

	return sessions[i].id;
}

void remove_session(SessionID id)
{
	assert(id != NO_SESSION);

	int i = 0;
	while (i < MAX_SESSIONS && sessions[i].id != id)
		i++;
	if (i == MAX_SESSIONS)
		log_fatal(LIT("Trying to remove non existing session"));
	sessions[i].id = NO_SESSION;
	sessions[i].name = NULLSTR;
	memset(sessions[i].namebuf, 0, sizeof(sessions[i].namebuf));
}

string name_from_session(SessionID id)
{
	assert(id != NO_SESSION);
	for (int i = 0; i < MAX_SESSIONS; i++)
		if (sessions[i].id == id)
			return sessions[i].name;
	return NULLSTR;
}

SessionID session_from_request(Request request)
{
	string sessid_str;
	if (!get_cookie(&request, LIT("sessid"), &sessid_str))
		return NO_SESSION;

	SessionID id;
	{
		char  *src = sessid_str.data;
		size_t len = sessid_str.size;
		size_t i = 0;

		while (i < len && is_space(src[i]))
			i++;

		if (i == len || !is_digit(src[i]))
			return NO_SESSION;
		uint32_t buf = 0;
		do {
			int d = src[i] - '0';
			if (buf > (UINT32_MAX - d) / 10)
				return NO_SESSION;
			buf = buf * 10 + d;
			i++;
		} while (i < len && is_digit(src[i]));

		while (i < len && is_space(src[i]))
			i++;

		if (i < len)
			return NO_SESSION;

		assert(sizeof(buf) == sizeof(SessionID));
		assert(buf != 0 && buf != NO_SESSION);
		id = buf;
	}

	return id;
}

///////////////////////////////////////////////////////////////////////////////////////////////
/// TEMPLATE EVALUATION                                                                     ///
///////////////////////////////////////////////////////////////////////////////////////////////