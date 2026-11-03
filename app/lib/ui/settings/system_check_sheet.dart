import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../l10n/l10n.dart';
import '../../runtime/runtime_bridge.dart';
import '../../runtime/runtime_state.dart';
import '../common/app_sheet.dart';
import '../theme/app_icons.dart';
import '../theme/semantic_colors.dart';

/// Runs the system container's requirement check and lists each result. With
/// [enabling], a passing check (warnings allowed) offers to turn the launch on
/// and the sheet returns true; anything else returns false.
Future<bool> showSystemCheckSheet(BuildContext context, {bool enabling = false}) async =>
    await showAppSheet<bool>(context, isScrollControlled: true, builder: (_) => SystemCheckSheet(enabling: enabling)) ??
    false;

class SystemCheckSheet extends ConsumerStatefulWidget {
  const SystemCheckSheet({super.key, this.enabling = false});

  final bool enabling;

  @override
  ConsumerState<SystemCheckSheet> createState() => _SystemCheckSheetState();
}

class _SystemCheckSheetState extends ConsumerState<SystemCheckSheet> {
  List<SystemCheckItem>? _items;
  bool _failed = false;

  @override
  void initState() {
    super.initState();
    final bridge = ref.read(runtimeBridgeProvider);
    bridge.checkSystemContainer().then(
      (items) {
        if (mounted) setState(() => _items = items);
      },
      onError: (Object _) {
        if (mounted) setState(() => _failed = true);
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    final l10n = context.l10n;
    final items = _items;
    final passed = items != null && items.isNotEmpty && items.every((item) => item.ok || item.warn);
    final warned = items != null && items.any((item) => !item.ok && item.warn);
    return SafeArea(
      top: false,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          SheetHeader(l10n.phoneSystemCheckTitle),
          Flexible(
            child: SingleChildScrollView(
              padding: const EdgeInsets.fromLTRB(24, 0, 24, 24),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  if (_failed)
                    Text(l10n.phoneSystemCheckError)
                  else if (items == null) ...[
                    const Center(child: CircularProgressIndicator()),
                    const SizedBox(height: 12),
                    Text(l10n.phoneSystemChecking, textAlign: TextAlign.center),
                  ] else ...[
                    for (final item in items) _Row(item),
                    const SizedBox(height: 12),
                    Text(
                      passed
                          ? warned
                                ? l10n.phoneSystemCheckPassedWarn
                                : l10n.phoneSystemCheckPassed
                          : l10n.phoneSystemCheckFailed,
                    ),
                    if (widget.enabling && passed) ...[
                      const SizedBox(height: 16),
                      FilledButton(
                        key: const Key('system-check-enable'),
                        onPressed: () => Navigator.of(context).pop(true),
                        child: Text(l10n.phoneSystemEnable),
                      ),
                    ],
                  ],
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _Row extends StatelessWidget {
  const _Row(this.item);

  final SystemCheckItem item;

  @override
  Widget build(BuildContext context) {
    final l10n = context.l10n;
    final colors = DshColors.of(context);
    final kernel = const {'mnt', 'pid', 'ipc', 'uts', 'user', 'cgroup'}.contains(item.key);
    final option = item.key.startsWith('kconfig:') ? item.key.substring('kconfig:'.length) : null;
    final title = switch (item.key) {
      'root' => l10n.phoneSystemCheckRoot,
      'mnt' => l10n.phoneSystemCheckMnt,
      'pid' => l10n.phoneSystemCheckPid,
      'ipc' => l10n.phoneSystemCheckIpc,
      'uts' => l10n.phoneSystemCheckUts,
      'user' => l10n.phoneSystemCheckUser,
      'noNewPrivs' => l10n.phoneSystemCheckNoNewPrivs,
      'trial' => l10n.phoneSystemCheckTrial,
      'cgroup' => l10n.phoneSystemCheckCgroupNs,
      'cgroup2' => l10n.phoneSystemCheckCgroup2,
      'launcher' => l10n.phoneSystemCheckLauncher,
      'image' => l10n.phoneSystemCheckImage,
      _ when option != null => 'CONFIG_$option',
      _ => item.key,
    };
    final detail = item.ok
        ? switch (item.key) {
            'root' => l10n.phoneSystemCheckRootOk,
            'noNewPrivs' => l10n.phoneSystemCheckNoNewPrivsOk,
            'trial' => l10n.phoneSystemCheckTrialOk,
            'cgroup2' => l10n.phoneSystemCheckCgroup2Ok,
            'launcher' => l10n.phoneSystemCheckLauncherOk,
            'image' => item.detail ?? '',
            _ when option != null => l10n.phoneSystemCheckOptionOn,
            _ => l10n.phoneSystemCheckKernelOk,
          }
        : switch (item.detail) {
            'off' => switch (option) {
              'CGROUP_PIDS' => l10n.phoneSystemCheckOffPids,
              'NF_TABLES' => l10n.phoneSystemCheckOffNftables,
              'BRIDGE_NETFILTER' => l10n.phoneSystemCheckOffBridgeNetfilter,
              'AUTOFS_FS' => l10n.phoneSystemCheckOffAutofs,
              'CHECKPOINT_RESTORE' => l10n.phoneSystemCheckOffCheckpoint,
              _ => l10n.phoneSystemCheckOptionOff,
            },
            'unsupported' when option != null => l10n.phoneSystemCheckOptionUnknown,
            'unsupported' when item.key == 'cgroup2' => l10n.phoneSystemCheckCgroup2Missing,
            'missing' when item.key == 'launcher' => l10n.phoneSystemCheckLauncherMissing,
            'missing' when item.key == 'image' => l10n.phoneSystemCheckImageMissing,
            'notGranted' => l10n.phoneSystemCheckNotGranted,
            'needsRoot' => l10n.phoneSystemCheckNeedsRoot,
            'unsupported' when kernel => l10n.phoneSystemCheckKernelMissing,
            'unsupported' when item.key == 'noNewPrivs' => l10n.phoneSystemCheckNoNewPrivsMissing,
            'skipped' => l10n.phoneSystemCheckSkipped,
            'escalated' => l10n.phoneSystemCheckEscalated,
            'timeout' => l10n.phoneSystemCheckTimeout,
            // The launcher's own failure line from the trial.
            final String raw => raw,
            null => '',
          };
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 8),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(
            item.ok
                ? AppIcons.success
                : item.warn
                ? AppIcons.warning
                : AppIcons.failed,
            size: 20,
            color: item.ok
                ? colors.success
                : item.warn
                ? colors.warning
                : colors.danger,
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(title, style: Theme.of(context).textTheme.bodyLarge),
                if (detail.isNotEmpty) Text(detail, style: Theme.of(context).textTheme.bodySmall),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
