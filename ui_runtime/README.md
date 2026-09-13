# Native UI screen router

This module is the engine-neutral routing/state seam for the native BF6 UI
viewer. It is deliberately separate from Dear ImGui and does not implement a
second hand-authored menu.

`DirectInstallScreenSource` owns a `bf6_ctx`, mounts the user's current Steam
install, enumerates the live `/screens/` EBX population and lazily builds a
requested screen from its Rime tree, shapes, images, text/property graphs and
event edges. It accepts no exported manifest, research TSV or derived JSON.

Minimal integration:

```cpp
std::string error;
auto source = bf6_ui::DirectInstallScreenSource::open(
    game_directory,
    bf6_ui::DirectInstallScreenSource::MountMode::WholeInstall,
    error);
bf6_ui::ScreenRouter router(*source, event_sink, offline_policy);
if (!router.refresh(error) || !router.push(exact_screen_path, error))
    return fail(error);

auto live = std::dynamic_pointer_cast<bf6_ui::DirectInstallScreenDocument>(
    router.current()->document);
paint(live->screen());
```

The renderer subscribes to `RouterEventSink`. Directional events contain the
verified BF6 native values `2/3/4/5` and retain the exact current focus address
if a decoded focus manager supplied one. The router does **not** infer the next
focus owner.

Back, Close and tab destinations are not established by the current native
evidence. Without a `NavigationPolicy`, those requests emit an unresolved event
and do not mutate the route stack. A tool-specific offline policy may explicitly
choose Push/Replace/Pop/Close; it must not be reported as a recovered BF6 law.

Boot-logo advancement has the same provider boundary. In the current Steam
install, the logo root's movie completion, `ConfirmSkip`, `ESCSkip`, and
authored skip-button sources converge on one exact output endpoint. That proves
when the root says it is done; the event graph contains no destination screen
identity. A native host should dispatch that exact authored edge on decoder EOS
and let a BootFlow provider choose the destination. An offline host may
explicitly `replace()` the logo with the exact catalogued
`bootflowstartscreen`, but must label that mapping as offline policy rather than
as recovered game routing. Do not use a wall-clock timeout or filename order.

Provider or evaluator values are written with `ScreenRouter::set_state` using
the exact partition, WidgetReference occurrence chain, local instance and
field id. `apply_supported_state` passes them through the existing decoded
Rime state bridge and leaves unknown renderer fields unresolved.

## General offline Rime runtime

`rime_runtime.{h,cpp}` is the engine-neutral execution layer. It reads a root
from the mounted install, uses the live recursive Rime tree to identify every
visual widget occurrence, then recursively mounts every
`LogicPrefabReferenceObjectData` occurrence. A repeated prefab always gets a
distinct occurrence path; local instance numbers are never treated as global.

The runtime owns no windowing or renderer code. Hosts supply exact provider
slots with `Runtime::set()` or a shipped interface field with
`Runtime::set_public()`, call `tick()`, and consume occurrence-scoped property
commits. `emit()` returns exact authored one-hop event deliveries. A
`DirectInstallScreenDocument` owns one such runtime and applies its initial
commits through the normal renderer bridge.

The executor currently admits the controlled native subset: property wires,
interface defaults, nested prefab pin bridges, ConditionalFloat,
ConditionalProperty, And, Or, Not, PropertyDefault, and fixed-duration,
game-clock Linear FloatInterpolator records.  Each admitted interpolator starts
at its authored `DefaultValue` and advances toward an exact resolved input over
its authored duration. Unknown inputs remain unknown,
equal-priority conflicts become ambiguous, cycles/depth limits are reported,
and ambiguous bidirectional interface pins are declined. The clock advances
monotonically. Nonlinear, velocity, dynamic-duration, real-time and
frame-correct interpolators, `LerpEntityData`, and general authored timeline
scheduling remain intentionally unresolved and are counted as declined; the
runtime does not substitute a guessed animation formula for those frames.

For controls, `authored_event_dispatch.{h,cpp}` is the exact one-hop event
boundary. A focus/hit-test/evaluator supplies the occurrence-scoped node and
the event it actually emitted; `exact_authored_fanout` returns every matching
shipped edge and no others. It deliberately does not recursively turn a target
input into a source output, because each target node's evaluator owns that
behavior and scheduling. Reaching an exact InterfaceDescriptor output is
reported as a provider handoff, never as an inferred route destination.

The two standalone tests are intentionally not wired into the shared CMake
file while another session owns that integration:

- `test/ui_screen_router_test.cpp` is the pure router/control gate.
- `test/ui_screen_router_install_test.cpp` is the direct Steam-install gate;
  it requires all 257 current `/screens/` paths, rejects a fake path and loads
  `MenuWeaponScreen` through the complete adapter.
- `test/ui_boot_route_install_test.cpp` proves the live logo completion/skip
  convergence, breaks it with a one-bit event mutation, validates both exact
  screen identities, and keeps logo-to-start as an explicit replace operation.
- `test/ui_authored_event_dispatch_test.cpp` proves exact source fan-out,
  duplicate-instance occurrence isolation, target/source polarity, mutated
  controls and exact provider-handoff classification.
