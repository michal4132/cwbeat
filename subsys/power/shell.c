/* SPDX-License-Identifier: Apache-2.0 */
#include <cwbeat/power.h>
#include <zephyr/shell/shell.h>
#include <string.h>
static int command(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (!strcmp(argv[0], "status")) {
		struct power_state state = power_snapshot();
		shell_print(shell, "intent=%u off=%u transition=%u error=%d", state.intent,
			    state.off_pending, state.transitioning, state.error);
		return 0;
	}
	enum power_command_kind kind = POWER_STOP;
	if (!strcmp(argv[0], "on")) {
		kind = POWER_ON;
	} else if (!strcmp(argv[0], "off")) {
		kind = POWER_OFF;
	} else if (!strcmp(argv[0], "reset")) {
		kind = POWER_RESET;
	} else if (!strcmp(argv[0], "recovery")) {
		kind = POWER_RECOVERY;
	}
	uint32_t id;
	int err = power_request(kind, &id);
	if (!err) {
		shell_print(shell, "power request %u", id);
	}
	return err;
}
SHELL_STATIC_SUBCMD_SET_CREATE(power_subcommands, SHELL_CMD(on, NULL, "Power on", command),
			       SHELL_CMD(off, NULL, "Power off", command),
			       SHELL_CMD(stop, NULL, "Stop peers", command),
			       SHELL_CMD(reset, NULL, "Reset board", command),
			       SHELL_CMD(recovery, NULL, "Reset into recovery", command),
			       SHELL_CMD(status, NULL, "Power intent", command),
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(power, &power_subcommands, "Board power", NULL);
