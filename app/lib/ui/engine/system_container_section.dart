import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:intl/intl.dart';

import '../../l10n/l10n.dart';
import '../../runtime/local_footprint.dart';
import '../../runtime/runtime_bridge.dart';
import '../../runtime/runtime_state.dart';
import '../common/brief_notice.dart';
import '../settings/settings_widgets.dart';
import '../theme/app_icons.dart';
import '../theme/semantic_colors.dart';

/// `s1-ubuntu-noble-SERIAL-HASH` → `s1 · SERIAL`; anything else is shown as is.
String shortSystemVersion(String? v) {
  if (v == null || v.isEmpty) return '—';
  final m = RegExp(r'^(s\d+)-ubuntu-[a-z]+-(\d{8}_\d{4})-[0-9a-f]+$').firstMatch(v);
  return m == null ? v : '${m.group(1)} · ${m.group(2)}';
}

/// "456 KB", "512 MB", "3.4 GB", "32 GB".
String formatStorageBytes(int bytes) {
  const gb = 1 << 30;
  if (bytes < 1 << 20) return '${(bytes / 1024).round()} KB';
  if (bytes < gb) return '${(bytes / (1 << 20)).round()} MB';
  final value = bytes / gb;
  return '${value < 10 && value != value.roundToDouble() ? value.toStringAsFixed(1) : value.round()} GB';
}

/// The system container (native SystemContainer), a daemon apart from the engine:
/// its state, start / stop / delete, the container storage, backup and restore, and
/// 「退出 DSH Go（含容器）」. Shown only when this build carries a system image.
class SystemContainerSection extends ConsumerStatefulWidget {
  const SystemContainerSection({super.key});

  @override
  ConsumerState<SystemContainerSection> createState() => _SystemContainerSectionState();
}

class _SystemContainerSectionState extends ConsumerState<SystemContainerSection> {
  bool _working = false;

  Future<void> _run(String action, {String? name, String? done}) async {
    setState(() => _working = true);
    try {
      await ref.read(runtimeBridgeProvider).systemContainer(action, name: name);
      if (done != null && mounted) showBriefNotice(context, done);
    } catch (_) {
      if (mounted) showBriefNotice(context, context.l10n.phoneSystemActionFailed);
    } finally {
      if (mounted) setState(() => _working = false);
    }
  }

  Future<void> _restore(List<SystemBackup> backups) async {
    final l10n = context.l10n;
    final colors = DshColors.of(context);
    final locale = Localizations.localeOf(context).toLanguageTag();
    final picked = await showDialog<SystemBackup>(
      context: context,
      builder: (context) => SimpleDialog(
        title: Text(l10n.phoneSystemRestore),
        children: [
          for (final b in backups)
            SimpleDialogOption(
              onPressed: () => Navigator.pop(context, b),
              child: ListTile(
                contentPadding: EdgeInsets.zero,
                title: Text(DateFormat.yMd(locale).add_Hm().format(DateTime.fromMillisecondsSinceEpoch(b.modified))),
                subtitle: Text('${b.name} · ${formatStorageBytes(b.bytes)}'),
              ),
            ),
        ],
      ),
    );
    if (picked == null || !mounted) return;
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(l10n.phoneSystemRestoreTitle),
        content: Text(l10n.phoneSystemRestoreBody),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: Text(l10n.commonCancel)),
          FilledButton(
            style: FilledButton.styleFrom(backgroundColor: colors.danger, foregroundColor: colors.onDanger),
            onPressed: () => Navigator.pop(context, true),
            child: Text(l10n.phoneSystemRestoreConfirm),
          ),
        ],
      ),
    );
    if (ok == true && mounted) await _run('restore', name: picked.name, done: l10n.phoneSystemRestoreDone);
  }

  Future<void> _setDockerTakeover(bool value) => _setOption(() => ref.read(runtimeBridgeProvider).setDockerTakeover(value));

  Future<void> _setLanAccess(bool value) => _setOption(() => ref.read(runtimeBridgeProvider).setLanAccess(value));

  Future<void> _setOption(Future<void> Function() change) async {
    setState(() => _working = true);
    try {
      await change();
    } catch (_) {
      if (mounted) showBriefNotice(context, context.l10n.phoneSystemActionFailed);
    } finally {
      if (mounted) setState(() => _working = false);
    }
  }

  Future<void> _delete() async {
    final l10n = context.l10n;
    final colors = DshColors.of(context);
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(l10n.phoneSystemDeleteTitle),
        content: Text(l10n.phoneSystemDeleteBody),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: Text(l10n.commonCancel)),
          FilledButton(
            style: FilledButton.styleFrom(backgroundColor: colors.danger, foregroundColor: colors.onDanger),
            onPressed: () => Navigator.pop(context, true),
            child: Text(l10n.phoneSystemDelete),
          ),
        ],
      ),
    );
    if (ok == true && mounted) await _run('delete');
  }

  @override
  Widget build(BuildContext context) {
    final l10n = context.l10n;
    final status = ref.watch(localRuntimeProvider).status;
    if (status.systemImage == null) return const SizedBox.shrink();
    final busy = _working || status.systemContainerBusy;
    final installed = status.systemImageInstalled != null;
    final running = status.systemContainerRunning;
    // Stopping the container would take the engine down with it: stop the engine first.
    final engineInside = status.systemEngineActive;
    final runtimeReady = !status.runtimeMissing && status.phase != RuntimePhase.installing;
    final storage = status.systemStorage;
    final backups = status.systemBackups;
    return SettingsSection(
      title: l10n.phoneSystemContainer,
      children: [
        InfoRow(
          label: l10n.phoneSystemState,
          value: busy
              ? l10n.phoneSystemBusy
              : running
              ? l10n.phoneSystemRunning
              : installed
              ? l10n.commonStopped
              : l10n.phoneSystemNotInstalled,
        ),
        InfoRow(
          label: l10n.phoneSystemImage,
          value: shortSystemVersion(status.systemImageInstalled ?? status.systemImage),
          mono: true,
        ),
        if (running)
          SettingsTile(
            title: l10n.phoneSystemStop,
            subtitle: engineInside ? l10n.phoneSystemStopEngineFirst : l10n.phoneSystemStopHint,
            chevron: false,
            onTap: busy || engineInside ? null : () => _run('stop'),
          )
        else
          SettingsTile(
            title: l10n.phoneSystemStart,
            subtitle: installed ? l10n.phoneSystemStartHint : l10n.phoneSystemStartInstallHint,
            chevron: false,
            onTap: busy || !runtimeReady ? null : () => _run('start'),
          ),
        InfoRow(
          label: l10n.phoneSystemStorage,
          value: storage == null
              ? l10n.phoneSystemStorageNone
              : l10n.phoneSystemStorageValue(formatStorageBytes(storage.used), formatStorageBytes(storage.ceiling)),
        ),
        if (storage != null)
          SettingsTile(
            title: l10n.phoneSystemGrow,
            subtitle: l10n.phoneSystemGrowHint,
            chevron: false,
            onTap: busy ? null : () => _run('grow'),
          ),
        if (installed)
          SettingsSwitchTile(
            title: l10n.phoneSystemDockerTakeover,
            subtitle: l10n.phoneSystemDockerHint,
            value: status.dockerTakeover,
            onChanged: busy ? null : _setDockerTakeover,
          ),
        if (installed)
          SettingsSwitchTile(
            title: l10n.phoneSystemLanAccess,
            subtitle: l10n.phoneSystemLanAccessHint,
            value: status.lanAccess,
            onChanged: busy ? null : _setLanAccess,
          ),
        if (installed)
          SettingsTile(
            icon: AppIcons.archive,
            tinted: false,
            title: l10n.phoneSystemBackup,
            subtitle: backups == null
                ? l10n.phoneSystemBackupNeedsStorage
                : engineInside
                ? l10n.phoneSystemStopEngineFirst
                : l10n.phoneSystemBackupHint,
            chevron: false,
            onTap: busy || backups == null || engineInside
                ? null
                : () => _run('backup', done: l10n.phoneSystemBackupDone),
          ),
        SettingsTile(
          icon: AppIcons.unarchive,
          tinted: false,
          title: l10n.phoneSystemRestore,
          subtitle: backups == null
              ? l10n.phoneSystemBackupNeedsStorage
              : backups.isEmpty
              ? l10n.phoneSystemRestoreNone
              : engineInside
              ? l10n.phoneSystemStopEngineFirst
              : l10n.phoneSystemRestoreHint,
          chevron: false,
          onTap: busy || backups == null || backups.isEmpty || engineInside || !runtimeReady
              ? null
              : () => _restore(backups),
        ),
        if (installed && !running)
          SettingsTile(
            icon: AppIcons.delete,
            tinted: false,
            title: l10n.phoneSystemDelete,
            subtitle: l10n.phoneSystemDeleteHint,
            danger: !busy,
            chevron: false,
            onTap: busy ? null : _delete,
          ),
        SettingsTile(
          icon: AppIcons.logout,
          tinted: false,
          title: l10n.phoneQuitAll,
          subtitle: l10n.phoneQuitAllHint,
          danger: true,
          chevron: false,
          onTap: () => ref.read(runtimeBridgeProvider).quit(),
        ),
      ],
    );
  }
}
