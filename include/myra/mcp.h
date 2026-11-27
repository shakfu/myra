/* mcp.h - stdio MCP servers whose tools the model can call. */
#ifndef MYRA_MCP_H
#define MYRA_MCP_H

#include "myra.h"

/* Start each server of a list in ACP's shape, [{name, command, args: [...], env:
   [{name, value}], type}], and add its tools to a->tools as mcp__<name>__<tool>. One
   that fails is reported on stderr and left out. Ctrl-C skips the rest. */
void myra_mcp_start_list(myra_agent *a, cJSON *servers);

/* The servers of a config file, {"mcpServers": {name: {command, args, env: {K: V}}}},
   as a list for myra_mcp_start_list; NULL with the reason on stderr. */
cJSON *myra_mcp_load(const char *path);

/* Stop every server and drop its tools. */
void myra_mcp_stop(myra_agent *a);

/* Whether name is a running server's tool. */
int myra_mcp_has(myra_agent *a, const char *name);

/* Run an MCP tool. The result is malloc'd, *err set if it failed. */
char *myra_mcp_call(myra_agent *a, const char *name, cJSON *input, int *err);

#endif
