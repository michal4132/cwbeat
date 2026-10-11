/* SPDX-License-Identifier: Apache-2.0 */
#include <cwbeat/bluetooth.h>
#include <cwbeat/audio.h>
#include <cwbeat/stock_peer.h>
#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>

#define RESULT_HISTORY_DEPTH 8
static struct k_spinlock history_lock;
static struct bluetooth_result history[RESULT_HISTORY_DEPTH];
static unsigned history_head, history_count;
static uint64_t history_total;

/* Diagnostic history is bounded and may overwrite. It never participates in
 * command admission or the owning module's completion retention. */
static void observe_result(const struct zbus_channel *channel)
{
	const struct bluetooth_result *result = zbus_chan_const_msg(channel);
	k_spinlock_key_t key = k_spin_lock(&history_lock);
	unsigned slot = (history_head + history_count) % RESULT_HISTORY_DEPTH;
	history[slot] = *result;
	if (history_count == RESULT_HISTORY_DEPTH) {
		history_head = (history_head + 1) % RESULT_HISTORY_DEPTH;
	} else {
		++history_count;
	}
	++history_total;
	k_spin_unlock(&history_lock, key);
}
ZBUS_LISTENER_DEFINE(bt_shell_result_observer, observe_result);
ZBUS_CHAN_ADD_OBS(bluetooth_results, bt_shell_result_observer, 99);

static int cmd_results(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct bluetooth_result snapshot[RESULT_HISTORY_DEPTH];
	k_spinlock_key_t key = k_spin_lock(&history_lock);
	unsigned count = history_count;
	uint64_t total = history_total;
	for (unsigned i = 0; i < count; ++i) {
		snapshot[i] = history[(history_head + i) % RESULT_HISTORY_DEPTH];
	}
	k_spin_unlock(&history_lock, key);
	shell_print(shell, "BT results observed=%llu retained=%u overwritten=%llu",
		    (unsigned long long)total, count, (unsigned long long)(total - count));
	for (unsigned i = 0; i < count; ++i) {
		const struct bluetooth_result *result = &snapshot[i];
		shell_print(shell, "id=%u session=%u command=%u status=%d", result->id,
			    result->session, result->command, result->status);
	}
	return 0;
}

static int cmd_status(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct bluetooth_state bt = bluetooth_snapshot();
	struct stock_peer_state peer = stock_peer_snapshot();
	struct audio_state audio = audio_snapshot();

	shell_print(shell,
		    "BT session=%u phase=%u readiness=%u connection=%u playing=%u "
		    "auxiliary=%u closed=%u error=%d",
		    bt.session, bt.phase, bt.readiness, bt.connection, bt.playing, bt.auxiliary,
		    bt.admission_closed, bt.error);
	shell_print(shell, "peer session=%u phase=%u closed=%u error=%d", peer.session, peer.phase,
		    peer.admission_closed, peer.error);
	shell_print(shell,
		    "audio session=%u controls=%u mode3=%u mode4=%u codec=%u revoked=%u "
		    "settled=%u quiescent=%u ready=%u error=%d",
		    audio.session, audio.controls_ready, audio.mode3_ready, audio.mode4_ready,
		    audio.codec_initialized, audio.raises_revoked, audio.callbacks_settled,
		    audio.quiescent, audio.ready, audio.error);
	return 0;
}

static int request_command(const struct shell *shell, enum bluetooth_command_kind command)
{
	uint32_t id;
	int status = bluetooth_request(command, &id);

	if (!status) {
		shell_print(shell, "Bluetooth request %u copied", id);
	}
	return status;
}

static int cmd_enable(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return request_command(shell, BLUETOOTH_ENABLE);
}

static int cmd_disable(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return request_command(shell, BLUETOOTH_DISABLE);
}

static int cmd_play(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return request_command(shell, BLUETOOTH_PLAY_PAUSE);
}

static int cmd_next(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return request_command(shell, BLUETOOTH_NEXT);
}

static int cmd_previous(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return request_command(shell, BLUETOOTH_PREVIOUS);
}

static int cmd_up(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return request_command(shell, BLUETOOTH_VOLUME_UP);
}

static int cmd_down(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return request_command(shell, BLUETOOTH_VOLUME_DOWN);
}

static int cmd_discover(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return request_command(shell, BLUETOOTH_DISCOVER);
}

static int cmd_pair(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return request_command(shell, BLUETOOTH_PAIR);
}

static int cmd_link(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return request_command(shell, BLUETOOTH_LINK);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	bt_subcommands,
	SHELL_CMD_ARG(disable, NULL, "Shut down Bluetooth; reboot before enabling again",
		      cmd_disable, 1, 0),
	SHELL_CMD_ARG(discover, NULL, "Request Bluetooth discovery", cmd_discover, 1, 0),
	SHELL_CMD_ARG(down, NULL, "Decrease volume", cmd_down, 1, 0),
	SHELL_CMD_ARG(enable, NULL, "Enable Bluetooth", cmd_enable, 1, 0),
	SHELL_CMD_ARG(link, NULL, "Request Bluetooth link", cmd_link, 1, 0),
	SHELL_CMD_ARG(next, NULL, "Skip to the next track", cmd_next, 1, 0),
	SHELL_CMD_ARG(pair, NULL, "Request Bluetooth pairing", cmd_pair, 1, 0),
	SHELL_CMD_ARG(play, NULL, "Toggle play/pause", cmd_play, 1, 0),
	SHELL_CMD_ARG(previous, NULL, "Skip to the previous track", cmd_previous, 1, 0),
	SHELL_CMD_ARG(results, NULL, "Show recent Bluetooth command results", cmd_results, 1, 0),
	SHELL_CMD_ARG(status, NULL, "Show Bluetooth, peer and audio state", cmd_status, 1, 0),
	SHELL_CMD_ARG(up, NULL, "Increase volume", cmd_up, 1, 0), SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(bt, &bt_subcommands, "Bluetooth commands", NULL);
