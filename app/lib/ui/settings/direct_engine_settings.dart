import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../l10n/l10n.dart';
import '../../runtime/runtime_bridge.dart';
import '../../runtime/runtime_state.dart';
import '../common/brief_notice.dart';
import 'system_check_sheet.dart';
import 'settings_widgets.dart';

/// Direct launch, the rooted namespace launch and the system container, all off
/// by default; a change applies only after a deliberate engine stop/start.
class DirectEngineSettings extends ConsumerStatefulWidget {
  const DirectEngineSettings({super.key});

  @override
  ConsumerState<DirectEngineSettings> createState() => _DirectEngineSettingsState();
}

class _DirectEngineSettingsState extends ConsumerState<DirectEngineSettings> {
  bool _saving = false;

  @override
  Widget build(BuildContext context) {
    final runtime = ref.watch(localRuntimeProvider);
    final status = runtime.status;
    final l10n = context.l10n;
    final stopped = !status.phase.isBusy && status.phase != RuntimePhase.running;
    final enabled =
        runtime.loaded && stopped && !_saving && (status.directEngineAvailable || status.directEngineEnabled);
    // Safe mode launches compatibly by design: not a sign the runtime needs it.
    // After Direct failed to launch, native reports the switch off until the
    // user turns it on again (a new try).
    final fallback = status.directEngineFallbackReason;
    final directFailed = fallback == 'directFailed';
    final runtimeNeedsCompatible = fallback != null && fallback != 'safeRecovery' && !directFailed;
    final hint = !status.directEngineAvailable
        ? l10n.phoneDirectNeedsRuntime
        : directFailed && !status.directEngineEnabled
        ? l10n.phoneDirectFailedHint
        : runtimeNeedsCompatible
        ? l10n.phoneDirectCompatibleHint
        : !stopped
        ? l10n.phoneDirectStopFirst
        : status.inSafeMode
        ? l10n.phoneDirectSafeHint
        : l10n.phoneDirectHint;
    final systemHint = status.systemEngineFailed && !status.systemEngineEnabled
        ? l10n.phoneSystemFailedHint
        : !stopped
        ? l10n.phoneDirectStopFirst
        : status.inSafeMode
        ? l10n.phoneDirectSafeHint
        : l10n.phoneSystemHint;

    return SettingsSection(
      title: l10n.phoneDeveloperOptions,
      children: [
        // Only after a direct launch failed: a fallback already runs compatibly
        // and the switch's hint says so.
        if (status.directEngineEnabled && status.launchSelectedMode == 'direct' && status.launchFailureReason != null)
          SettingsTile(
            title: l10n.phoneReturnCompatible,
            subtitle: stopped ? null : l10n.phoneDirectStopFirst,
            onTap: enabled ? () => _change(false) : null,
          ),
        SettingsSwitchTile(
          title: l10n.phoneDirectEngine,
          subtitle: hint,
          value: status.directEngineEnabled,
          onChanged: enabled ? _change : null,
        ),
        // Only in builds that carry a system image (arm64).
        if (status.systemImage != null) ...[
          SettingsSwitchTile(
            title: l10n.phoneSystemEngine,
            subtitle: systemHint,
            value: status.systemEngineEnabled,
            onChanged: runtime.loaded && stopped && !_saving ? _changeSystem : null,
          ),
          SettingsTile(
            title: l10n.phoneSystemCheck,
            subtitle: l10n.phoneSystemCheckHint,
            onTap: runtime.loaded && !_saving ? () => showSystemCheckSheet(context) : null,
          ),
        ],
      ],
    );
  }

  Future<void> _change(bool value) => _save(() => ref.read(runtimeBridgeProvider).setDirectEngine(value));

  /// Turning it on goes through the requirement check: on only through a pass (warnings allowed).
  Future<void> _changeSystem(bool value) async {
    if (value && !await showSystemCheckSheet(context, enabling: true)) return;
    if (!mounted) return;
    await _save(() => ref.read(runtimeBridgeProvider).setSystemEngine(value));
  }

  Future<void> _save(Future<void> Function() change) async {
    setState(() => _saving = true);
    try {
      await change();
    } catch (_) {
      if (mounted) showBriefNotice(context, context.l10n.phoneDirectChangeFailed);
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }
}
