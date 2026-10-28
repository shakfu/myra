/* acp.h - serve the Agent Client Protocol: the CLI's --acp mode. */
#ifndef MYRA_ACP_H
#define MYRA_ACP_H

#include "myra.h"

/* Serve ACP on stdin and stdout until stdin closes. */
int myra_acp(myra_agent *a);

#endif
