/*-------------------------------------------------------------------------
 *
 * pg_plan_advsr.c
 *		Automatic plan tuning by correcting row count estimation errors 
 *		using a feedback loop between planner and executor.
 *
 * Copyright (c) 2019, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 * IDENTIFICATION
 *	  pg_plan_advsr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "executor/executor.h"
#include "tcop/utility.h"
#include "nodes/nodeFuncs.h"
#include "nodes/extensible.h"

#include "commands/explain.h"
#include "commands/prepare.h"
#include "common/md5.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/ruleutils.h"

#include "access/hash.h"

#include "libpq-int.h"
/*
#include "guc.h"
*/
/* came from pg_hint_plan REL10_1_3_2 */
#include "normalize_query.h"

/* came from pg_store_plans 1.3 */
#include "pgsp_json.h"

PG_MODULE_MAGIC;


bool isExplain;

/* for leading hint */
typedef struct LeadingContext
{
	StringInfo lead_str;
	ExplainState *es;
} LeadingContext;

/* scan method/join method/leading hints */
static StringInfo scan_str;
static StringInfo join_str;
static StringInfo rows_str;
LeadingContext *leadcxt;

/* hash value made by queryDesc->sourceText */
static uint32 pgsp_queryid;
/* hash value made by normalized_plan */
static uint32 pgsp_planid;

/* estimated/actual rows number */
static double est_rows;
static double act_rows;
static double diff_rows; /* = act_rows - est_rows */
static double total_diff_rows;

/* counters */
static int scan_cnt;
static int join_cnt;
static int rows_cnt;

/* This is made by generate_normalized_query in post_parse_analyze_hook */
char *normalized_query;
/* Current nesting depth of ExecutorRun+ProcessUtility calls */
static int	nested_level = 0;

/* Connect String */
/*
static const char *connstr = "host=127.0.0.1 port=5151 dbname=postgres";
*/
static StringInfo connstr;

/* GUC variables */
/* enabling / disabling pg_plan_advsr during EXPLAIN ANALYZE */
static bool pg_plan_advsr_is_enabled;
/* enabling / disable quiet mode */
static bool pg_plan_advsr_is_quieted;

/* Saved hook values in case of unload */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static ProcessUtility_hook_type		prev_ProcessUtility_hook = NULL;
static ExecutorStart_hook_type		prev_ExecutorStart_hook = NULL;
static ExecutorRun_hook_type		prev_ExecutorRun_hook = NULL;
static ExecutorFinish_hook_type		prev_ExecutorFinish_hook = NULL;
static ExecutorEnd_hook_type		prev_ExecutorEnd_hook = NULL;

void		_PG_init(void);
void		_PG_fini(void);

PG_FUNCTION_INFO_V1(pg_plan_advsr_enable_feedback);
Datum       pg_plan_advsr_enable_feedback(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_plan_advsr_disable_feedback);
Datum       pg_plan_advsr_disable_feedback(PG_FUNCTION_ARGS);

/* Hook functions for pg_plan_advsr */
static void pg_plan_advsr_post_parse_analyze_hook(ParseState *pstate, Query *query);
static void pg_plan_advsr_ProcessUtility_hook(PlannedStmt *pstmt,
											  const char *queryString,
											  ProcessUtilityContext context,
											  ParamListInfo params,QueryEnvironment *queryEnv,
											  DestReceiver *dest,
											  char *completionTag);
static void pg_plan_advsr_ExecutorStart_hook(QueryDesc *queryDesc, int eflags);
static void pg_plan_advsr_ExecutorRun_hook(QueryDesc *queryDesc, ScanDirection direction,
										   uint64 count, bool execute_once);
static void pg_plan_advsr_ExecutorFinish_hook(QueryDesc *queryDesc);
static void pg_plan_advsr_ExecutorEnd_hook(QueryDesc *queryDesc);

/* Utility functions */
static bool pg_plan_advsr_query_walker(Node *parsetree);

/* This function came from pg_hint_plan.c */
static const char *
get_query_string(ParseState *pstate, Query *query, Query **jumblequery);

ExplainState * pg_plan_advsr_NewExplainState(void);

/* entry point of pg_plan_advsr */
void pg_plan_advsr_ExplainPrintPlan(ExplainState *es, QueryDesc *queryDesc);

void CreateScanJoinRowsHints(PlanState *planstate, List *ancestors,
							 const char *relationship, const char *plan_name,
							 ExplainState *es);

/* This function called by planstate_tree_walker for creating Leading Hints */
bool CreateLeadingHint(PlanState *planstate, LeadingContext *lead);

void store_info_to_tables(double totaltime, const char *sourcetext); /* store query, hints and diff to tables */

void get_rows_hint_from_table(const char *conninfo,
							  const char *norm_query_str,
							  StringInfo prev_rows_hint,
							  char *aplname);

/* these functions based on explain.c */
bool ExplainPreScanNode(PlanState *planstate, Bitmapset **rels_used);

bool pg_plan_advsr_planstate_tree_walker(PlanState *planstate,
										 bool (*walker) (),
										 void *context);
bool pg_plan_advsr_planstate_walk_subplans(List *plans,
										   bool (*walker) (),
										   void *context);
bool pg_plan_advsr_planstate_walk_members(List *plans, 
										  PlanState **planstates,
										  bool (*walker) (),
										  void *context);

void pg_plan_advsr_ExplainSubPlans(List *plans, List *ancestors,
								   const char *relationship, ExplainState *es);

void pg_plan_advsr_ExplainScanTarget(Scan *plan, ExplainState *es);
void pg_plan_advsr_ExplainTargetRel(Plan *plan, Index rti, ExplainState *es);

char *get_relnames(ExplainState *es, Relids relids);
char *get_target_relname(Index rti, ExplainState *es);

/* inspired from pg_store_plans.c */
uint32 create_pgsp_planid(QueryDesc *queryDesc);
/* came from pg_store_plans.c */
static uint32 hash_query(const char* query);

/* Create connstr and Execute query */
void get_connstr(StringInfo str);
bool execSQL(const char *conninfo, const char *sql, int nParams, const char **params);
bool execSQL_simple(const char *conninfo, const char *sql);

/* replace all before strings to after strings in buf strings */
void replaceAll(char *buf, const char *before, const char *after);

/* SQL */
/* This is for pg_plan_advsr.feedback option */
#define FEEDBACK_OPTION_SQL "\
set pg_plan_advsr.enabled to on; \
set pg_hint_plan.debug_print to on; \
set pg_hint_plan.enable_hint_table to on; "

/* For inserting hints, plans and query to tables */
#define INSERT_PLAN_HISTORY_SQL "\
insert into plan_repo.plan_history (\
norm_query_hash, \
pgsp_queryid, \
pgsp_planid, \
execution_time, \
rows_hint, \
scan_hint, \
join_hint, \
lead_hint, \
diff_of_joins, \
join_cnt, \
application_name, \
timestamp) \
values ('%s', '%u', '%u', %0.3f, '%s', '%s', '%s', '%s', %.0f, '%d', '%s', current_timestamp)"

#define INSERT_NORM_QUERIES_SQL "\
INSERT INTO plan_repo.norm_queries (\
norm_query_hash, \
norm_query_string) \
VALUES ('%s', '%s')"

#define INSERT_RAW_QUERIES_SQL "\
INSERT INTO plan_repo.raw_queries (\
norm_query_hash, \
raw_query_string, \
timestamp) \
VALUES ('%s', '%s', current_timestamp)"

/* For inserting rows hints to table to use it for auto tuning */
#define SELECT_HINT_SQL "\
select hints from hint_plan.hints \
where norm_query_string = '%s' \
and application_name = '%s'"

/* Delete previous rows_hints */
#define DELETE_ROWS_HINT_SQL "\
delete from hint_plan.hints \
where norm_query_string = '%s' \
and application_name = '%s'"

/* Insert new rows_hint */
#define INSERT_NEW_ROWS_HINT_SQL "\
insert into hint_plan.hints (\
norm_query_string, \
hints, \
application_name) \
values ('%s', '%s', '%s')"


/* Install hooks */
void _PG_init(void)
{
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pg_plan_advsr_post_parse_analyze_hook;

	prev_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = pg_plan_advsr_ProcessUtility_hook;

	prev_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = pg_plan_advsr_ExecutorStart_hook;

	prev_ExecutorRun_hook = ExecutorRun_hook;
	ExecutorRun_hook = pg_plan_advsr_ExecutorRun_hook;

	prev_ExecutorFinish_hook = ExecutorFinish_hook;
	ExecutorFinish_hook = pg_plan_advsr_ExecutorFinish_hook;
	
	prev_ExecutorEnd_hook = ExecutorEnd_hook;
	ExecutorEnd_hook = pg_plan_advsr_ExecutorEnd_hook;

	DefineCustomBoolVariable("pg_plan_advsr.enabled",
							 "Enable / Disable pg_plan_advsr",
							 NULL,
							 &pg_plan_advsr_is_enabled,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_plan_advsr.quieted",
							 "Enable / Disable pg_plan_advsr's quiet mode",
							 NULL,
							 &pg_plan_advsr_is_quieted,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
}

/* Uninstall hooks. */
void _PG_fini(void)
{
	post_parse_analyze_hook = prev_post_parse_analyze_hook;
	ProcessUtility_hook = prev_ProcessUtility_hook;
	ExecutorStart_hook = prev_ExecutorStart_hook;
	ExecutorRun_hook = prev_ExecutorRun_hook;
	ExecutorFinish_hook = prev_ExecutorFinish_hook;
	ExecutorEnd_hook = prev_ExecutorEnd_hook;
}

/*
 * Enable feedback loop.
 *
 * Following GUC parameters will became "ON" when this function executed.
 *   pg_plan_advsr.enabled
 *   pg_hint_plan.debug_print
 *   pg_hint_plan.enable_hint_table
 */
Datum
pg_plan_advsr_enable_feedback(PG_FUNCTION_ARGS)
{
	elog(DEBUG3, "execute pg_plan_advsr_enable_feedback");

	(void) set_config_option("pg_plan_advsr.enabled", "ON",
							  PGC_USERSET, PGC_S_OVERRIDE,
							  GUC_ACTION_SAVE, true, 0, false);

	(void) set_config_option("pg_hint_plan.enable_hint_table", "ON",
							  PGC_USERSET, PGC_S_OVERRIDE,
							  GUC_ACTION_SAVE, true, 0, false);

	(void) set_config_option("pg_hint_plan.debug_print", "ON",
							  PGC_USERSET, PGC_S_OVERRIDE,
							  GUC_ACTION_SAVE, true, 0, false);
	PG_RETURN_VOID();
}

/* 
 * Disable feedback loop 
 */
Datum
pg_plan_advsr_disable_feedback(PG_FUNCTION_ARGS)
{
	elog(DEBUG3, "execute pg_plan_advsr_disable_feedback");

	(void) set_config_option("pg_plan_advsr.enabled", "ON",
							  PGC_USERSET, PGC_S_OVERRIDE,
							  GUC_ACTION_SAVE, true, 0, false);

	(void) set_config_option("pg_hint_plan.enable_hint_table", "OFF",
							  PGC_USERSET, PGC_S_OVERRIDE,
							  GUC_ACTION_SAVE, true, 0, false);

	(void) set_config_option("pg_hint_plan.debug_print", "OFF",
							  PGC_USERSET, PGC_S_OVERRIDE,
							  GUC_ACTION_SAVE, true, 0, false);
	PG_RETURN_VOID();
}


/* To get normalized query like a pg_hint_plan.c */
static void
pg_plan_advsr_post_parse_analyze_hook(ParseState *pstate, Query *query)
{
	const char *query_str;

	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query);

	/* Create normalized query for later use */
	if (pg_plan_advsr_is_enabled)
	{
		int				query_len;
		pgssJumbleState	jstate;
		Query		   *jumblequery;
		/*
		elog(INFO, "##pg_plan_advsr_post_parse_analyze_hook start ##");
		*/
		query_str = get_query_string(pstate, query, &jumblequery);

		if (query_str && jumblequery)
		{
			/*
			 * XXX: normalizing code is copied from pg_stat_statements.c, so be
			 * careful to PostgreSQL's version up.
			 */
			jstate.jumble = (unsigned char *) palloc(JUMBLE_SIZE);
			jstate.jumble_len = 0;
			jstate.clocations_buf_size = 32;
			jstate.clocations = (pgssLocationLen *)
				palloc(jstate.clocations_buf_size * sizeof(pgssLocationLen));
			jstate.clocations_count = 0;

			JumbleQuery(&jstate, jumblequery);

			/*
			 * Normalize the query string by replacing constants with '?'
			 */
			/*
			 * Search hint string which is stored keyed by query string
			 * and application name.  The query string is normalized to allow
			 * fuzzy matching.
			 *
			 * Adding 1 byte to query_len ensures that the returned string has
			 * a terminating NULL.
			 */
			query_len = strlen(query_str) + 1;
			normalized_query =
				generate_normalized_query(&jstate, query_str,
										  query->stmt_location,
										  &query_len,
										  GetDatabaseEncoding());

		}
		/*
		elog(INFO, "##pg_plan_advsr_post_parse_analyze_hook end ##");
		*/
	}
}


/*
 * This function setup the "isExplain" flag.
 * If this flag is true, we can create hints for given query.
 */
static void pg_plan_advsr_ProcessUtility_hook(PlannedStmt *pstmt,
											  const char *queryString,
											  ProcessUtilityContext context,
											  ParamListInfo params,
											  QueryEnvironment *queryEnv,
											  DestReceiver *dest,
											  char *completionTag)
{
	isExplain = query_or_expression_tree_walker((Node *) pstmt,
												pg_plan_advsr_query_walker,
												NULL,
												0);

	if (prev_ProcessUtility_hook)
		prev_ProcessUtility_hook(
								 pstmt,
								 queryString,
								 context,
								 params,
								 queryEnv,
								 dest, 
								 completionTag);
	else
		standard_ProcessUtility(
								pstmt,
								queryString,
								context,
								params,
								queryEnv,
								dest, 
								completionTag);
}

/* ExecutorStart, Run and Finish are came from pg_store_plans.c */
/*
 * ExecutorStart hook: start up tracking if needed
 */
static void
pg_plan_advsr_ExecutorStart_hook(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart_hook)
		prev_ExecutorStart_hook(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * Set up to track total elapsed time in ExecutorRun. Allocate in per-query
	 * context so as to be free at ExecutorEnd.
	 */
	if (queryDesc->totaltime == NULL && (nested_level == 0))
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
		queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL);
		MemoryContextSwitchTo(oldcxt);
	}
	
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pg_plan_advsr_ExecutorRun_hook(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
				 bool execute_once)
{
	nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun_hook)
			prev_ExecutorRun_hook(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
		nested_level--;
	}
	PG_CATCH();
	{
		nested_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
pg_plan_advsr_ExecutorFinish_hook(QueryDesc *queryDesc)
{
	nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish_hook)
			prev_ExecutorFinish_hook(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		nested_level--;
	}
	PG_CATCH();
	{
		nested_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd_hook: create hints by using PlannedStmt or ExplainState, and output it.
 */
static void pg_plan_advsr_ExecutorEnd_hook(QueryDesc *queryDesc)
{
	ExplainState *hs;

	if(queryDesc->totaltime)
		InstrEndLoop(queryDesc->totaltime);

	elog(DEBUG1, "isExplain: %d", isExplain);
	if(isExplain && pg_plan_advsr_is_enabled)
	{
		elog(DEBUG1, "## pg_plan_advsr_ExecutorEnd start ##");

		/* Create Hints using HintState like a ExplainState */
		hs = pg_plan_advsr_NewExplainState();
		hs->analyze = true;
		hs->verbose = true;
		hs->buffers = false;
		hs->timing  = false;
		hs->summary = hs->analyze;
		hs->format  = EXPLAIN_FORMAT_JSON;

		pg_plan_advsr_ExplainPrintPlan(hs, queryDesc);

		elog(DEBUG1, "## pg_plan_advsr_ExecutorEnd end ##");

	}

	/* initialize */
	isExplain = false;
	
	if(prev_ExecutorEnd_hook)
		prev_ExecutorEnd_hook(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * Create pg_store_plans's planid
 * This function is inspired store_entry() and pgsp_ExecutorEnd() in pg_store_plans 1.1.
 */
uint32 create_pgsp_planid(QueryDesc *queryDesc)
{
	ExplainState *es     = NewExplainState();
	StringInfo    es_str = es->str;
	bool log_verbose = false;
	bool log_buffers = false;
	bool log_timing  = false; 
	bool log_triggers = false;
	char       *normalized_plan = NULL;
	uint32      planid;         /* plan identifier */

	/* get the current values of pg_store_plans setting */
	/* GetConfigOptionByName("pg_store_plans.log_analyze", NULL, false); */
	if(strcmp(GetConfigOptionByName("pg_store_plans.log_verbose", NULL, false), "on") == 0)
		log_verbose = true;
	if(strcmp(GetConfigOptionByName("pg_store_plans.log_buffers", NULL, false), "on") == 0)
		log_buffers = true;
	if(strcmp(GetConfigOptionByName("pg_store_plans.log_timing",  NULL, false), "on") == 0)
		log_timing = true;
	if(strcmp(GetConfigOptionByName("pg_store_plans.log_triggers",NULL, false), "on") == 0) 
		log_triggers = true;
	
	/* elog(INFO, "pg_store_plans.log_analyze: %s", GetConfigOptionByName("pg_store_plans.log_analyze", NULL, false)); */
	elog(DEBUG1, "pg_store_plans.log_verbose : %s", log_verbose ? "on" : "off");
	elog(DEBUG1, "pg_store_plans.log_buffers : %s", log_buffers ? "on" : "off");
	elog(DEBUG1, "pg_store_plans.log_timing  : %s", log_timing  ? "on" : "off");
	elog(DEBUG1, "pg_store_plans.log_triggers: %s", log_triggers? "on" : "off");

	es->analyze = queryDesc->instrument_options;
	es->verbose = log_verbose;
	es->buffers = (es->analyze && log_buffers);
	es->timing  = (es->analyze && log_timing);
	es->format  = EXPLAIN_FORMAT_JSON;

	ExplainBeginOutput(es);
	ExplainPrintPlan(es, queryDesc);
	/* Trigger is not supported */
	/*
	if (log_triggers)
		pgspExplainTriggers(es, queryDesc);
	*/
	ExplainEndOutput(es);

	/* Remove last line break */
	if (es_str->len > 0 && es_str->data[es_str->len - 1] == '\n')
		es_str->data[--es_str->len] = '\0';

	/* JSON outmost braces. */
	es_str->data[0] = '{';
	es_str->data[es_str->len - 1] = '}';

	/* es_str->data is a Plan formatted Json */
	normalized_plan = pgsp_json_normalize(es_str->data);
	planid = hash_any((const unsigned char *)normalized_plan,
						strlen(normalized_plan));
	elog(DEBUG3, "normalized_plan: %s", normalized_plan);
	pfree(normalized_plan);

	elog(DEBUG3, "planid: %u", planid);
	return planid;
}

/* This function cames from pg_store_plans 1.1. */
static uint32
hash_query(const char* query)
{
	uint32 queryid;

	char *normquery = pstrdup(query);
	normalize_expr(normquery, false);
	queryid = hash_any((const unsigned char*)normquery, strlen(normquery));
	pfree(normquery);

	return queryid;
}



/*
 * Detect if the current utility command is EXPLAIN with ANALYZE option.
 */
static bool
pg_plan_advsr_query_walker(Node *parsetree)
{
	if (parsetree == NULL)
		return false;

	parsetree = ((PlannedStmt *) parsetree)->utilityStmt;
	if (parsetree == NULL)
		return false;

	switch (nodeTag(parsetree))
	{
		case T_ExplainStmt:
			{
				ListCell   *lc;

				foreach(lc, ((ExplainStmt *) parsetree)->options)
				{
					DefElem    *opt = (DefElem *) lfirst(lc);

					if (strcmp(opt->defname, "analyze") == 0)
						return true;
				}
			}
			return false;
			break;
		default:
			return false;
	}
	return false;
}

/* This function came from pg_plan_hint.c */
/*
 * Get client-supplied query string. Addtion to that the jumbled query is
 * supplied if the caller requested. From the restriction of JumbleQuery, some
 * kind of query needs special amendments. Reutrns NULL if this query doesn't
 * change the current hint. This function returns NULL also when something
 * wrong has happend and let the caller continue using the current hints.
 */
static const char *
get_query_string(ParseState *pstate, Query *query, Query **jumblequery)
{
	const char *p = debug_query_string;

	if (!p)
	{
		if (!pstate->p_sourcetext)
			return NULL;

		p = pstate->p_sourcetext;
	}

	if (jumblequery != NULL)
		*jumblequery = query;

	if (query->commandType == CMD_UTILITY)
	{
		Query *target_query = (Query *)query->utilityStmt;

		/*
		 * Some CMD_UTILITY statements have a subquery that we can hint on.
		 * Since EXPLAIN can be placed before other kind of utility statements
		 * and EXECUTE can be contained other kind of utility statements, these
		 * conditions are not mutually exclusive and should be considered in
		 * this order.
		 */
		if (IsA(target_query, ExplainStmt))
		{
			ExplainStmt *stmt = (ExplainStmt *)target_query;
			
			Assert(IsA(stmt->query, Query));
			target_query = (Query *)stmt->query;

			/* strip out the top-level query for further processing */
			if (target_query->commandType == CMD_UTILITY &&
				target_query->utilityStmt != NULL)
				target_query = (Query *)target_query->utilityStmt;
		}

		if (IsA(target_query, DeclareCursorStmt))
		{
			DeclareCursorStmt *stmt = (DeclareCursorStmt *)target_query;
			Query *query = (Query *)stmt->query;

			/* the target must be CMD_SELECT in this case */
			Assert(IsA(query, Query) && query->commandType == CMD_SELECT);
			target_query = query;
		}

		if (IsA(target_query, CreateTableAsStmt))
		{
			CreateTableAsStmt  *stmt = (CreateTableAsStmt *) target_query;

			Assert(IsA(stmt->query, Query));
			target_query = (Query *) stmt->query;

			/* strip out the top-level query for further processing */
			if (target_query->commandType == CMD_UTILITY &&
				target_query->utilityStmt != NULL)
				target_query = (Query *)target_query->utilityStmt;
		}

		if (IsA(target_query, ExecuteStmt))
		{
			/*
			 * Use the prepared query for EXECUTE. The Query for jumble
			 * also replaced with the corresponding one.
			 */
			ExecuteStmt *stmt = (ExecuteStmt *)target_query;
			PreparedStatement  *entry;

			entry = FetchPreparedStatement(stmt->name, true);
			p = entry->plansource->query_string;
			target_query = (Query *) linitial (entry->plansource->query_list);
		}
			
		/* JumbleQuery accespts only a non-utility Query */
		if (!IsA(target_query, Query) ||
			target_query->utilityStmt != NULL)
			target_query = NULL;

		if (jumblequery)
			*jumblequery = target_query;
	}
	/* Return NULL if the pstate is not identical to the top-level query */
	else if (strcmp(pstate->p_sourcetext, p) != 0)
		p = NULL;

	return p;
}

/*  
 * pg_plan_advsr_planstate_tree_walker, pg_plan_advsr_planstate_walk_subplans and
 * pg_plan_advsr_planstate_walk_members came from src/backend/nodes/nodeFuncs.c
 */

/* This function is only for current node, lefttree and righttree */
/*
 * planstate_tree_walker --- walk plan state trees
 *
 * The walker has already visited the current node, and so we need only
 * recurse into any sub-nodes it has.
 */
bool
pg_plan_advsr_planstate_tree_walker(PlanState *planstate,
									bool (*walker) (),
									void *context)
{

	/* lefttree */
	if (outerPlanState(planstate))
	{
		if (walker(outerPlanState(planstate), context))
			return true;
	}

	/* righttree */
	if (innerPlanState(planstate))
	{
		if (walker(innerPlanState(planstate), context))
			return true;
	}

	/* Todo: 
			investigate these node whether it is needed for creating hints or not.
	*/
	/* special child plans */
	/*
	Plan	   *plan = planstate->plan;

	switch (nodeTag(plan))
	{
		case T_ModifyTable:
			if (planstate_walk_members(((ModifyTable *) plan)->plans,
									   ((ModifyTableState *) planstate)->mt_plans,
									   walker, context))
				return true;
			break;
		case T_Append:
			if (planstate_walk_members(((Append *) plan)->appendplans,
									   ((AppendState *) planstate)->appendplans,
									   walker, context))
				return true;
			break;
		case T_MergeAppend:
			if (planstate_walk_members(((MergeAppend *) plan)->mergeplans,
									   ((MergeAppendState *) planstate)->mergeplans,
									   walker, context))
				return true;
			break;
		case T_BitmapAnd:
			if (planstate_walk_members(((BitmapAnd *) plan)->bitmapplans,
									   ((BitmapAndState *) planstate)->bitmapplans,
									   walker, context))
				return true;
			break;
		case T_BitmapOr:
			if (planstate_walk_members(((BitmapOr *) plan)->bitmapplans,
									   ((BitmapOrState *) planstate)->bitmapplans,
									   walker, context))
				return true;
			break;
		case T_SubqueryScan:
			if (walker(((SubqueryScanState *) planstate)->subplan, context))
				return true;
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScanState *) planstate)->custom_ps)
			{
				if (walker((PlanState *) lfirst(lc), context))
					return true;
			}
			break;
		default:
			break;
	}
	*/

	/* subPlan-s */
	/*
	if (planstate_walk_subplans(planstate->subPlan, walker, context))
		return true;
	*/

	return false;
}

/*
 * Walk a list of SubPlans (or initPlans, which also use SubPlan nodes).
 */
bool
pg_plan_advsr_planstate_walk_subplans(List *plans,
									  bool (*walker) (),
									  void *context)
{
	ListCell   *lc;

	foreach(lc, plans)
	{
		SubPlanState *sps = lfirst_node(SubPlanState, lc);

		if (walker(sps->planstate, context))
			return true;
	}

	return false;
}

/*
 * Walk the constituent plans of a ModifyTable, Append, MergeAppend,
 * BitmapAnd, or BitmapOr node.
 *
 * Note: we don't actually need to examine the Plan list members, but
 * we need the list in order to determine the length of the PlanState array.
 */
bool
pg_plan_advsr_planstate_walk_members(List *plans, PlanState **planstates,
									 bool (*walker) (), void *context)
{
	int			nplans = list_length(plans);
	int			j;

	for (j = 0; j < nplans; j++)
	{
		if (walker(planstates[j], context))
			return true;
	}

	return false;
}

/* return relnames */
char *get_relnames(ExplainState *es, Relids relids)
{
	int x;
	int first = true;
	StringInfo relnames = makeStringInfo();

	x = -1;
	while ((x = bms_next_member(relids, x)) >= 0)
	{
		if(!first)
			appendStringInfo(relnames, " %s", get_target_relname(x, es));
		else
			appendStringInfo(relnames, "%s", get_target_relname(x, es));
		first = false;
	}
	return relnames->data;
}

/*
 * ExplainPreScanNode -
 *    Prescan the planstate tree to identify which RTEs are referenced
 *
 * Adds the relid of each referenced RTE to *rels_used.  The result controls
 * which RTEs are assigned aliases by select_rtable_names_for_explain.
 * This ensures that we don't confusingly assign un-suffixed aliases to RTEs
 * that never appear in the EXPLAIN output (such as inheritance parents).
 */
bool
ExplainPreScanNode(PlanState *planstate, Bitmapset **rels_used)
{
	Plan	*plan = planstate->plan;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
			*rels_used = bms_add_member(*rels_used,
										 ((Scan *) plan)->scanrelid);
			break;
		case T_ForeignScan:
			*rels_used = bms_add_members(*rels_used,
										 ((ForeignScan *) plan)->fs_relids);
			break;
		case T_CustomScan:
			*rels_used = bms_add_members(*rels_used,
										 ((CustomScan *) plan)->custom_relids);
			break;
		case T_ModifyTable:
			*rels_used = bms_add_member(*rels_used,
										 ((ModifyTable *) plan)->nominalRelation);
			if (((ModifyTable *) plan)->exclRelRTI)
				*rels_used = bms_add_member(*rels_used,
											((ModifyTable *) plan)->exclRelRTI);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExplainPreScanNode, rels_used);
}

/* For creating Leading hint */
bool
CreateLeadingHint(PlanState *planstate, LeadingContext *lead)
{
	Plan	*plan = planstate->plan;

	/* skip initPlans and subPlans*/
	if(planstate->initPlan || planstate->subPlan)
		return false;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_CteScan:
		case T_BitmapHeapScan:
		case T_FunctionScan:
			elog(DEBUG1, "seqscan: %u", ((Scan *) plan)->scanrelid);
			appendStringInfo(lead->lead_str, "%s ", get_target_relname(((Scan *) plan)->scanrelid, lead->es));
			break;
		case T_IndexScan:
		case T_IndexOnlyScan:
			elog(DEBUG1, "indscan: %u", ((Scan *) plan)->scanrelid);
			appendStringInfo(lead->lead_str, "%s ", get_target_relname(((Scan *) plan)->scanrelid, lead->es));
			break;
		case T_HashJoin:
			elog(DEBUG1, "HJ(");
			appendStringInfo(lead->lead_str, "(");
			pg_plan_advsr_planstate_tree_walker(planstate, CreateLeadingHint, lead);
			appendStringInfo(lead->lead_str, ")");
			elog(DEBUG1, ")");
			break;
		case T_NestLoop:
			elog(DEBUG1, "NL(");
			appendStringInfo(lead->lead_str, "(");
			pg_plan_advsr_planstate_tree_walker(planstate, CreateLeadingHint, lead);
			appendStringInfo(lead->lead_str, ")");
			elog(DEBUG1, ")");
			break;
		case T_MergeJoin:
			elog(DEBUG1, "MJ(");
			appendStringInfo(lead->lead_str, "(");
			pg_plan_advsr_planstate_tree_walker(planstate, CreateLeadingHint, lead);
			appendStringInfo(lead->lead_str, ")");
			elog(DEBUG1, ")");
			break;
		default:
			pg_plan_advsr_planstate_tree_walker(planstate, CreateLeadingHint, lead);
	}
	return false;
}

/* This function is entory point */
/*
 * pg_plan_advsr_ExplainPrintPlan -
 *    convert a QueryDesc's plan tree to text and append it to es->str
 *
 * The caller should have set up the options fields of *es, as well as
 * initializing the output buffer es->str.  Also, output formatting state
 * such as the indent level is assumed valid.  Plan-tree-specific fields
 * in *es are initialized here.
 *
 * NB: will not work on utility statements
 */
void
pg_plan_advsr_ExplainPrintPlan(ExplainState *es, QueryDesc *queryDesc)
{
	Bitmapset  *rels_used = NULL;
	PlanState  *ps;
	double totaltime;

	total_diff_rows = 0;

	/* Set up ExplainState fields associated with this plan tree */
	Assert(queryDesc->plannedstmt != NULL);
	es->pstmt = queryDesc->plannedstmt;
	es->rtable = queryDesc->plannedstmt->rtable;

	ExplainPreScanNode(queryDesc->planstate, &rels_used);

	es->rtable_names = select_rtable_names_for_explain(es->rtable, rels_used);
	es->deparse_cxt = deparse_context_for_plan_rtable(es->rtable,
													  es->rtable_names);
	es->printed_subplans = NULL;

	/*
	 * Sometimes we mark a Gather node as "invisible", which means that it's
	 * not displayed in EXPLAIN output.  The purpose of this is to allow
	 * running regression tests with force_parallel_mode=regress to get the
	 * same results as running the same tests with force_parallel_mode=off.
	 */
	ps = queryDesc->planstate;
	if (IsA(ps, GatherState) &&((Gather *) ps->plan)->invisible)
		ps = outerPlanState(ps);

	/* Create scan_str, join_str, rows_str */
	CreateScanJoinRowsHints(ps, NIL, NULL, NULL, es);

	/* Create leading_str */
	leadcxt = (LeadingContext *) palloc0(sizeof(LeadingContext));
	leadcxt->lead_str = makeStringInfo();
	leadcxt->es = es;

	appendStringInfo(leadcxt->lead_str, "LEADING( ");
	elog(DEBUG1, "### CreateLeadingHint ###");
	CreateLeadingHint(ps, leadcxt);
	appendStringInfo(leadcxt->lead_str, " )");

	/* queryId is made by pg_stat_statements */
	pgsp_queryid = hash_query(queryDesc->sourceText);
	pgsp_planid = create_pgsp_planid(queryDesc);
	totaltime = queryDesc->totaltime ? queryDesc->totaltime->total * 1000.0 : 0;

	if ( ! pg_plan_advsr_is_quieted )
	{
		elog(INFO, "---- pgsp_queryid ----------------\n\t\t%u", pgsp_queryid);
		elog(INFO, "---- pgsp_planid -----------------\n\t\t%u", pgsp_planid);
		elog(INFO, "---- Execution Time --------------\n\t\t%0.3f ms", totaltime);
		elog(DEBUG3, "---- Query text -------------------\n%s\n", queryDesc->sourceText);
		/* normalized_query is made by post_parse_analyze_hook function */
		elog(DEBUG3, "---- Normalized query text --------\n%s\n", normalized_query);
		elog(INFO, "---- Hints for current plan ------\n%s\n%s\n%s", scan_str->data, join_str->data, leadcxt->lead_str->data);
		elog(INFO, "---- Rows hint (feedback info)----\n%s", rows_str->data);
		elog(INFO, "---- Join count ------------------\n\t\t%d", join_cnt);
		elog(INFO, "---- Total diff rows of joins ----\n\t\t%.0f", total_diff_rows);
	}

	/* store above data to tables */
	/* 
		ToDo: 
				- Execute these queries in one transaction
				- Use Prepared Statement
				- Avoid "Not found table error"
	*/
	store_info_to_tables(totaltime, queryDesc->sourceText);

	/* initialize */
	normalized_query = NULL;
	pfree(leadcxt);
}

/*
 * Store query, hints and diffs to tables
 *
 * 
 */
void store_info_to_tables(double totaltime, const char *sourcetext)
{
	char md5[33];
	char *aplname;
	StringInfo sql;
	StringInfo del_sql;
	StringInfo prev_rows_hint;
	StringInfo new_hint;

	char *before = "'";
	char *after  = "\'\'";
	char *output;

	/*
	 * Calculate MD5 hash of the normalized query as a norm_query_hash 
	 */
	if (! pg_md5_hash(normalized_query, strlen(normalized_query), md5))
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("pg_md5_hash: out of memory")));
	}

	/* get connect string */
	connstr = makeStringInfo();
	get_connstr(connstr);
	elog(DEBUG3, "connstr->data: %s", connstr->data);

	/* get application_name */
	aplname = GetConfigOptionByName("application_name", NULL, false);
	if ( ! pg_plan_advsr_is_quieted )
		elog(INFO, "---- Application name-------------\n\t\t%s", aplname);

	/* insert totaltime and hints to plan_repo.plan_history */
	sql = makeStringInfo();
	if (!rows_str->data)
		rows_str->data = "";
	appendStringInfo(sql, INSERT_PLAN_HISTORY_SQL,
									md5,
									pgsp_queryid,
									pgsp_planid,
									totaltime,
									rows_str->data,
									scan_str->data,
									join_str->data,
									leadcxt->lead_str->data,
									total_diff_rows, /* diff of joins */
									join_cnt,
									aplname);
	elog(DEBUG3, "#plan_repo.plan_history sql: \n%s", sql->data);

	if(!execSQL_simple(connstr->data, sql->data))
		elog(INFO, "\ninsert error: plan_history\n");
	else
	{
		elog(DEBUG3, "\ninsert success: plan_history\n");
	}

	/* insert queryhash and normalized query text to plan_repo.norm_queries */
	sql = makeStringInfo();
	appendStringInfo(sql, INSERT_NORM_QUERIES_SQL,
									md5,
									normalized_query);

	elog(DEBUG3, "#plan_repo.norm_queries sql: \n%s", sql->data);
	if(!execSQL_simple(connstr->data, sql->data))
		elog(INFO, "\ninsert error: norm_queries\n");
	else
	{
		elog(DEBUG3, "\ninsert success: norm_queries\n");
	}

	/* insert queryhash and raw query text to plan_repo.raw_queries */
	/* should be replaced "'" to "''" in sql */
	output = (char *)palloc((int)strlen(sourcetext) * 1.5);
	strcpy(output, sourcetext);
	elog(DEBUG3, "output(before): %s", output);
	replaceAll(output, before, after);
	elog(DEBUG3, "output(after): %s", output);

	sql = makeStringInfo();
	appendStringInfo(sql, INSERT_RAW_QUERIES_SQL,
									md5,
									output);

	elog(DEBUG3, "#plan_repo.raw_queries sql: \n%s", sql->data);
	if(!execSQL_simple(connstr->data, sql->data))
		elog(INFO, "\ninsert error: raw_queries\n");
	else
	{
		elog(DEBUG3, "\ninsert success: raw_queries\n");
	}
	pfree(output);

	/* upsert hints to hint_plan.hints */
	sql = makeStringInfo();
	del_sql = makeStringInfo();
	prev_rows_hint = makeStringInfo();
	new_hint = makeStringInfo();

	/* get previous rows_hint from table */
	get_rows_hint_from_table(connstr->data, normalized_query, prev_rows_hint, aplname);
	
	if (prev_rows_hint)
	{
		/* delete previous rows_hint */
		appendStringInfo(del_sql, DELETE_ROWS_HINT_SQL, normalized_query, aplname);
		if(!execSQL_simple(connstr->data, del_sql->data))
			elog(INFO, "\ndelete error: hint_plan.hints\n");
		else
		{
			elog(DEBUG3, "\ndelete success: hint_plan.hints\n");
		}
		/* create new rows_hint */
		appendStringInfo(new_hint, "%s %s", prev_rows_hint->data, rows_str->data);
	}
	else
	{
		appendStringInfo(new_hint, "%s", rows_str->data);
	}

	/* insert new rows_hint to table for auto tune */
	appendStringInfo(sql, INSERT_NEW_ROWS_HINT_SQL, normalized_query, new_hint->data, aplname);
	if(!execSQL_simple(connstr->data, sql->data))
		elog(INFO, "\ninsert error: hint_plan.hints\n");
	else
	{
		elog(DEBUG3, "\ninsert success: hint_plan.hints\n");
	}
	
}




/*
 * Get target relation name of a scan
 */
char *
get_target_relname(Index rti, ExplainState *es)
{
	RangeTblEntry *rte;
	char		  *refname;

	elog(DEBUG1, "    #get_target_relname#");

	rte = rt_fetch(rti, es->rtable);
	refname = (char *) list_nth(es->rtable_names, rti - 1);
	if (refname == NULL)
		refname = rte->eref->aliasname;

	return refname;
}


/* 
 * Create scan, join and rows hints.
 * This function is based on ExplainNode in explain.c
 */
/*
 * CreateScanJoinRowsHints -
 *	  Appends a description of a plan tree to es->str *
 * planstate points to the executor state node for the current plan node.
 * We need to work from a PlanState node, not just a Plan node, in order to
 * get at the instrumentation data (if any) as well as the list of subplans.
 *
 * ancestors is a list of parent PlanState nodes, most-closely-nested first.
 * These are needed in order to interpret PARAM_EXEC Params.
 *
 * relationship describes the relationship of this plan node to its parent
 * (eg, "Outer", "Inner"); it can be null at top level.  plan_name is an
 * optional name to be attached to the node.
 *
 */
void
CreateScanJoinRowsHints(PlanState *planstate, List *ancestors,
			const char *relationship, const char *plan_name,
			ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	bool		haschildren;
	double		nloops;
	double		rows;
	StringInfo  tmp_relnames = makeStringInfo();

	elog(DEBUG1, "### CreateScanJoinRowsHints ###");
	elog(DEBUG1, "    Parent Relationship: %s", relationship);


	/* Remove initPlan-s such as CTE */
	if (planstate->initPlan)
	{
		if ( ! pg_plan_advsr_is_quieted )
			elog(INFO, "---- InitPlan -----------------");
		/* Remove it for leading hint */
		planstate->initPlan = NULL;
		/*
		pg_plan_advsr_ExplainSubPlans(planstate->initPlan, ancestors, "InitPlan", es);
		*/
		if ( ! pg_plan_advsr_is_quieted )
			elog(INFO, "-------------------------------");
	}

	/* Also remove subPlan-s */
	if (planstate->subPlan)	
	{
		if ( ! pg_plan_advsr_is_quieted )
			elog(INFO, "---- SubPlan ------------------");
		planstate->subPlan = NULL;
		if ( ! pg_plan_advsr_is_quieted )
			elog(INFO, "-------------------------------");
	}

	/* Create scan hints using ExplainScanTarget */
	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
			pg_plan_advsr_ExplainScanTarget((Scan *) plan, es);
			break;
		case T_ForeignScan:
		case T_CustomScan:
			if (((Scan *) plan)->scanrelid > 0)
				pg_plan_advsr_ExplainScanTarget((Scan *) plan, es);
			break;
		case T_IndexScan:
			{
				IndexScan  *indexscan = (IndexScan *) plan;
				pg_plan_advsr_ExplainScanTarget((Scan *) indexscan, es);
			}
			break;
		case T_IndexOnlyScan:
			{
				IndexOnlyScan *indexonlyscan = (IndexOnlyScan *) plan;
				pg_plan_advsr_ExplainScanTarget((Scan *) indexonlyscan, es);
			}
			break;
		case T_BitmapIndexScan:
			{
				/*
				BitmapIndexScan *bitmapindexscan = (BitmapIndexScan *) plan;
				*/
			}
			break;
		case T_ModifyTable:
			break;
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			break;
		default:
			break;
	}

	/*
	 * We have to forcibly clean up the instrumentation state because we
	 * haven't done ExecutorEnd yet.  This is pretty grotty ...
	 *
	 * Note: contrib/auto_explain could cause instrumentation to be set up
	 * even though we didn't ask for it here.  Be careful not to print any
	 * instrumentation results the user didn't ask for.  But we do the
	 * InstrEndLoop call anyway, if possible, to reduce the number of cases
	 * auto_explain has to contend with.
	 */
	if (planstate->instrument)
		InstrEndLoop(planstate->instrument);

	if (planstate->instrument)
	{
		/* EXPLAIN ANALYZE */
		nloops = planstate->instrument->nloops;
		rows = planstate->instrument->ntuples / nloops; /* actual rows */
	}
	else
	{
		/* EXPLAIN */
		rows = -1;
	}
	/* 
	 * Create join and rows hints.
	 * In this current design, we use actual rows number as a rows hint.
	 */
	switch(nodeTag(plan))
	{
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			{
				Bitmapset  *relids = NULL;

				ExplainPreScanNode(planstate, &relids);
				tmp_relnames->data = get_relnames(es, relids);

				if (join_cnt > 0)
					appendStringInfo(join_str, "\n");

				if (nodeTag(plan) == T_NestLoop)
					appendStringInfo(join_str, "%s", "NESTLOOP");
				else if (nodeTag(plan) == T_MergeJoin)
					appendStringInfo(join_str, "%s", "MERGEJOIN");
				else if (nodeTag(plan) == T_HashJoin)
					appendStringInfo(join_str, "%s", "HASHJOIN");

				appendStringInfo(join_str, "(%s) ", tmp_relnames->data);
				join_cnt++;

				est_rows = ((Plan *)planstate->plan)->plan_rows; /* estimated rows */
				act_rows = rows == -1 ? est_rows : rows;
				diff_rows = act_rows - est_rows; /* diff rows = actual rows - estimated rows */

				if(est_rows != act_rows)
				{
					if (rows_cnt > 0)
						appendStringInfo(rows_str, "\n");
					appendStringInfo(rows_str, "ROWS(%s #%.0f) ", tmp_relnames->data, act_rows);
					rows_cnt++;
				}

				if(diff_rows < 0)
					diff_rows = diff_rows * -1.0;
				elog(DEBUG3, "join diff_rows: %.0f", diff_rows);
				total_diff_rows = total_diff_rows + diff_rows;
			}
			break;
		default:
			break;
	}

	/* Get ready to display the child plans */
	haschildren = planstate->initPlan ||
		outerPlanState(planstate) ||
		innerPlanState(planstate) ||
		IsA(plan, ModifyTable) ||
		IsA(plan, Append) ||
		IsA(plan, MergeAppend) ||
		IsA(plan, BitmapAnd) ||
		IsA(plan, BitmapOr) ||
		IsA(plan, SubqueryScan) ||
		(IsA(planstate, CustomScanState) &&
		 ((CustomScanState *) planstate)->custom_ps != NIL) ||
		planstate->subPlan;
	if (haschildren)
	{
		/* Pass current PlanState as head of ancestors list for children */
		ancestors = lcons(planstate, ancestors);
	}

	/* lefttree */
	if (outerPlanState(planstate))
		CreateScanJoinRowsHints(outerPlanState(planstate), ancestors,
					"Outer", NULL, es);

	/* righttree */
	if (innerPlanState(planstate))
		CreateScanJoinRowsHints(innerPlanState(planstate), ancestors,
					"Inner", NULL, es);

	if (haschildren)
	{
		ancestors = list_delete_first(ancestors);
	}
}


/*
 * Create a new ExplainState struct initialized with default options.
 */
ExplainState *
pg_plan_advsr_NewExplainState(void)
{
	ExplainState *es = (ExplainState *) palloc0(sizeof(ExplainState));

	elog(DEBUG1, "### pg_plan_advsr_NewExplainState ###");

	/* Set default options (most fields can be left as zeroes). */
	es->costs = true;
	/* Prepare output buffer. */
	es->str = makeStringInfo();

	scan_str = makeStringInfo();
	join_str = makeStringInfo();
	rows_str = makeStringInfo();
	est_rows = 0;
	act_rows = 0;
	diff_rows = 0;
	scan_cnt = 0;
	join_cnt = 0;
	rows_cnt = 0;

	return es;
}

/*
 * Show the target of a Scan node
 */
void
pg_plan_advsr_ExplainScanTarget(Scan *plan, ExplainState *es)
{
	pg_plan_advsr_ExplainTargetRel((Plan *) plan, plan->scanrelid, es);
}



/*
 * Show the target relation of a scan or modify node
 */
void
pg_plan_advsr_ExplainTargetRel(Plan *plan, Index rti, ExplainState *es)
{
	RangeTblEntry *rte;
	char	   *refname;

	elog(DEBUG1, "    # pg_plan_advsr_ExplainTargetRel #");

	rte = rt_fetch(rti, es->rtable);
	refname = (char *) list_nth(es->rtable_names, rti - 1);
	if (refname == NULL)
		refname = rte->eref->aliasname;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_CteScan:
		case T_FunctionScan:
			if (scan_cnt > 0)
				if (scan_cnt % 5 == 0)
					appendStringInfo(scan_str, "\n");
			appendStringInfo(scan_str, "SEQSCAN(%s) ", quote_identifier(refname));
			scan_cnt++;
			break;
		case T_BitmapHeapScan:
			if (scan_cnt > 0)
				if (scan_cnt % 5 == 0)
					appendStringInfo(scan_str, "\n");
			appendStringInfo(scan_str, "BITMAPSCAN(%s) ", quote_identifier(refname));
			scan_cnt++;
			break;
		case T_IndexScan:
			if (scan_cnt > 0)
				if (scan_cnt % 5 == 0)
					appendStringInfo(scan_str, "\n");
			appendStringInfo(scan_str, "INDEXSCAN(%s) ", quote_identifier(refname));
			scan_cnt++;
			break;
		case T_IndexOnlyScan:
			if (scan_cnt > 0)
				if (scan_cnt % 5 == 0)
					appendStringInfo(scan_str, "\n");
			appendStringInfo(scan_str, "INDEXONLYSCAN(%s) ", quote_identifier(refname));
			scan_cnt++;
			break;
		default:
			break;
	}
}

/*
 * Insert plans, queries and hints to tables using PQexecParams
 */
/*
bool
execSQL(const char *conninfo, const char *sql, int nParams, const char **params)
{
	PGconn		*con;
	PGresult 	*res;

	if ((con = PQconnectdb(conninfo)) == NULL)
	{
		ereport(LOG,
				(errmsg("could not establish conenction to server: \"%s\"",
					conninfo)));

		PQfinish(con);
		return false;
	}

	res = PQexecParams(conninfo, sql, nParams, NULL, params, NULL, NULL, 0);

	switch (PQresultStatus(res))
	{
		case PGRES_TUPLES_OK:
		case PGRES_COMMAND_OK:
			break;
		default:
			ereport(LOG,
					(errmsg("query failed: \"%s\"", sql)));
			break;
	}

	PQfinish(con);
	return true;
}
*/

/*
 * Insert plans, queries and hints to tables using PQexec
 */
bool
execSQL_simple(const char *conninfo, const char *sql)
{
	PGconn		*con;
	PGresult 	*res;

	/* Try to connect to primary server */
	if ((con = PQconnectdb(conninfo)) == NULL)
	{
		ereport(LOG,
				(errmsg("could not establish conenction to server: \"%s\"",
					conninfo)));

		PQfinish(con);
		return false;
	}

	res = PQexec(con, sql);

	switch (PQresultStatus(res))
	{
		case PGRES_TUPLES_OK:
		case PGRES_COMMAND_OK:
			break;
		default:
			ereport(LOG,
					(errmsg("query failed: \"%s\"", sql)));
			PQfinish(con);
			return false;
	}

	PQfinish(con);
	return true;
}

/* 
 * get rows_hint from hint_plan.hints table
 */
void get_rows_hint_from_table(const char *conninfo, const char *norm_query_str, StringInfo prev_rows_hint, char *aplname)
{
	PGconn    *con;
	PGresult  *res;
	int        hint_fnum;
	char      *hint;
	StringInfo sql = makeStringInfo();
	hint = NULL;

	/* Try to connect to primary server */
	if ((con = PQconnectdb(conninfo)) == NULL)
	{
		ereport(LOG,
				(errmsg("could not establish conenction to server: \"%s\"",
					conninfo)));

		PQfinish(con);
		return;
	}

	appendStringInfo(sql, SELECT_HINT_SQL, norm_query_str, aplname); 
	res = PQexec(con, sql->data);

	switch (PQresultStatus(res))
	{
		case PGRES_TUPLES_OK:
		case PGRES_COMMAND_OK:
			elog(DEBUG3, "res->ntups: %d", res->ntups);
			elog(DEBUG3, "res->numAttributes: %d", res->numAttributes);
			hint_fnum = PQfnumber(res, "hints");
			hint = PQgetvalue(res, 0, hint_fnum);
			elog(DEBUG3, "prev_rows_hint: %s", hint);
			if (hint)
				appendStringInfo(prev_rows_hint, "%s", hint);
			
			PQclear(res);
			PQfinish(con);
			break;
		default:
			ereport(LOG,
					(errmsg("query failed: \"%s\"", sql->data)));
			if(res)
				PQclear(res);
			PQfinish(con);
	}
}

/*
 * Replace all before strings to after strings in buf strings.
 * NOTE: Assumes there is enough room in the buf buffer!
 */
void replaceAll(char *buf, const char *before, const char *after)
{
	char *dup = pstrdup(buf);	// copy buf, and malloc
	char *ptr1, *ptr2;

	int dup_len = strlen(dup);
	int before_len = strlen(before);
	int after_len = strlen(after);

	ptr1 = dup;
	while ((ptr2 = strstr(ptr1, before)) != NULL)
	{
		strncpy(buf, ptr1, ptr2 - ptr1);
		buf += (ptr2 - ptr1);

		strncpy(buf, after, after_len);
		buf += after_len;

		ptr1 = ptr2 + before_len;
	}
	strncpy(buf, ptr1, dup + dup_len - ptr1);
	buf += (dup + dup_len - ptr1);
	strcpy(buf, "\0");

	pfree(dup);
}

/* Create connstr from env */
void get_connstr(StringInfo str)
{
	char *env;
	char *pghost = "";
	char *pgport = "";
	char *login = NULL;
	char *dbName;

	if ((env = getenv("PGHOST")) != NULL && *env != '\0')
		pghost = env;
	else
		pghost = "127.0.0.1";

	if ((env = getenv("PGPORT")) != NULL && *env != '\0')
		pgport = env;
	else
		pgport = "5432";

	if ((env = getenv("PGUSER")) != NULL && *env != '\0')
		login = env;
	else
		login = "postgres";

	if ((env = getenv("PGDATABASE")) != NULL && *env != '\0')
		dbName = env;
	else
		dbName = "postgres";

	/*
		*connstr = "host=127.0.0.1 port=5151 dbname=postgres";
	*/
	appendStringInfo(str, "host=%s port=%s dbname=%s user=%s", pghost, pgport, dbName, login);
}

#include "pg_stat_statements.c"

