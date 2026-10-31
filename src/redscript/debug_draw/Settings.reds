// Debug Draw's own options. With Mod Settings installed they are on its page under "Debug Draw"; without it the
// defaults below apply. They act on every mod's shapes (show / hide, text size, label distance, dim strength) and can
// draw the world overlays around V with no scripting.
public class DebugDrawSettings extends IScriptable {
  @runtimeProperty("ModSettings.mod", "Debug Draw")
  @runtimeProperty("ModSettings.category", "Display")
  @runtimeProperty("ModSettings.displayName", "Show debug drawing")
  @runtimeProperty("ModSettings.description", "Off hides everything every mod draws with Debug Draw, without uninstalling anything")
  public let visible: Bool = true;

  @runtimeProperty("ModSettings.mod", "Debug Draw")
  @runtimeProperty("ModSettings.category", "Display")
  @runtimeProperty("ModSettings.displayName", "Text size")
  @runtimeProperty("ModSettings.description", "Multiplies the size of all debug text and labels")
  @runtimeProperty("ModSettings.step", "0.1")
  @runtimeProperty("ModSettings.min", "0.5")
  @runtimeProperty("ModSettings.max", "3.0")
  @runtimeProperty("ModSettings.dependency", "visible")
  public let textScale: Float = 1.0;

  @runtimeProperty("ModSettings.mod", "Debug Draw")
  @runtimeProperty("ModSettings.category", "Display")
  @runtimeProperty("ModSettings.displayName", "Label distance")
  @runtimeProperty("ModSettings.description", "Multiplies how far away labels with a distance limit still show")
  @runtimeProperty("ModSettings.step", "0.25")
  @runtimeProperty("ModSettings.min", "0.25")
  @runtimeProperty("ModSettings.max", "4.0")
  @runtimeProperty("ModSettings.dependency", "visible")
  public let labelDistance: Float = 1.0;

  @runtimeProperty("ModSettings.mod", "Debug Draw")
  @runtimeProperty("ModSettings.category", "Display")
  @runtimeProperty("ModSettings.displayName", "Dim strength")
  @runtimeProperty("ModSettings.description", "How much mods may darken the scene to make their shapes stand out. 0 turns dimming off")
  @runtimeProperty("ModSettings.step", "0.05")
  @runtimeProperty("ModSettings.min", "0.0")
  @runtimeProperty("ModSettings.max", "1.0")
  @runtimeProperty("ModSettings.dependency", "visible")
  public let dimStrength: Float = 1.0;

  @runtimeProperty("ModSettings.mod", "Debug Draw")
  @runtimeProperty("ModSettings.category", "World overlays")
  @runtimeProperty("ModSettings.displayName", "Show overlays around V")
  @runtimeProperty("ModSettings.description", "Draws the overlays chosen below around V, every frame")
  public let overlays: Bool = false;

  // The toggle key (Input Loader, src/input_loader/debug_draw.xml) is off for now: merging that mapping crashed the
  // game during load (2026-10-31), and Mod Settings' key overrides currently find no mod's mapping. Not on the page
  // until it works; the plugin does not register inputs.xml (Main.cpp).
  public let debugDrawOverlaysToggle: EInputKey = EInputKey.IK_None;

  @runtimeProperty("ModSettings.mod", "Debug Draw")
  @runtimeProperty("ModSettings.category", "World overlays")
  @runtimeProperty("ModSettings.displayName", "Collision")
  @runtimeProperty("ModSettings.description", "Physics shapes: static in pale green, moving in orange")
  public let collision: Bool = true;

  @runtimeProperty("ModSettings.mod", "Debug Draw")
  @runtimeProperty("ModSettings.category", "World overlays")
  @runtimeProperty("ModSettings.displayName", "Navmesh")
  @runtimeProperty("ModSettings.description", "Where V and NPCs can walk: cyan, outer edges brighter")
  public let navmesh: Bool = true;

  @runtimeProperty("ModSettings.mod", "Debug Draw")
  @runtimeProperty("ModSettings.category", "World overlays")
  @runtimeProperty("ModSettings.displayName", "Triggers")
  @runtimeProperty("ModSettings.description", "Trigger volumes, coloured by kind: quest yellow, player green, sound blue, others magenta")
  public let triggers: Bool = false;

  @runtimeProperty("ModSettings.mod", "Debug Draw")
  @runtimeProperty("ModSettings.category", "World overlays")
  @runtimeProperty("ModSettings.displayName", "Radius")
  @runtimeProperty("ModSettings.description", "Metres around V. Collision uses this radius, the navmesh and triggers 1.5 times it")
  @runtimeProperty("ModSettings.step", "5.0")
  @runtimeProperty("ModSettings.min", "5.0")
  @runtimeProperty("ModSettings.max", "60.0")
  public let radius: Float = 20.0;

  @runtimeProperty("ModSettings.mod", "Debug Draw")
  @runtimeProperty("ModSettings.category", "World overlays")
  @runtimeProperty("ModSettings.displayName", "Draw through walls")
  public let throughWalls: Bool = false;
}

// Owns the settings and pushes them to the plugin when they change (and when V is attached); listens for the toggle
// key. The plugin draws the overlays around V itself, every frame.
public class DebugDrawSettingsSystem extends ScriptableSystem {
  private let settings: ref<DebugDrawSettings>;

  public static func GetInstance(game: GameInstance) -> ref<DebugDrawSettingsSystem> {
    return GameInstance.GetScriptableSystemsContainer(game).Get(n"DebugDrawSettingsSystem") as DebugDrawSettingsSystem;
  }

  public func GetSettings() -> ref<DebugDrawSettings> {
    return this.settings;
  }

  private func OnAttach() -> Void {
    this.settings = new DebugDrawSettings();
    DebugDraw_ListenToSettings(this.settings, this);
    this.Apply();
  }

  private func OnDetach() -> Void {
    DebugDraw_StopListeningToSettings(this.settings, this);
  }

  private func OnPlayerAttach(request: ref<PlayerAttachRequest>) -> Void {
    let player = GameInstance.GetPlayerSystem(this.GetGameInstance()).GetLocalPlayerMainGameObject();
    if IsDefined(player) {
      // runs on every player attach: unregister first so a press only toggles once
      player.UnregisterInputListener(this, n"DebugDraw_ToggleOverlays");
      player.RegisterInputListener(this, n"DebugDraw_ToggleOverlays");
    }
    this.Apply();
  }

  protected cb func OnAction(action: ListenerAction, consumer: ListenerActionConsumer) -> Bool {
    if Equals(ListenerAction.GetName(action), n"DebugDraw_ToggleOverlays") && ListenerAction.IsButtonJustPressed(action) {
      this.ToggleOverlays();
    }
  }

  // Mod Settings has written the new values into this.settings.
  public func OnModSettingsChange() -> Void {
    this.Apply();
  }

  // Flips "Show overlays around V", and saves it through Mod Settings (when installed) so its page stays in step.
  public func ToggleOverlays() -> Void {
    this.settings.overlays = !this.settings.overlays;
    DebugDraw_SaveSetting(n"World overlays", n"overlays", this.settings.overlays);
    this.Apply();
  }

  public func Apply() -> Void {
    let s = this.settings;
    DebugDraw_SetDisplay(s.visible, s.textScale, s.labelDistance, s.dimStrength);
    let kinds = 0;
    if s.overlays {
      kinds = (s.navmesh ? 1 : 0) + (s.triggers ? 2 : 0) + (s.collision ? 4 : 0);
    }
    let player = GameInstance.GetPlayerSystem(this.GetGameInstance()).GetLocalPlayerMainGameObject();
    DebugDraw_SetWorldOverlays(player, kinds, s.radius, s.throughWalls);
  }
}

// Mod Settings is optional: these compile to nothing without it.
@if(ModuleExists("ModSettingsModule"))
public func DebugDraw_ListenToSettings(settings: ref<IScriptable>, system: ref<IScriptable>) {
  ModSettings.RegisterListenerToClass(settings);
  ModSettings.RegisterListenerToModifications(system);
}

@if(!ModuleExists("ModSettingsModule"))
public func DebugDraw_ListenToSettings(settings: ref<IScriptable>, system: ref<IScriptable>) {}

@if(ModuleExists("ModSettingsModule"))
public func DebugDraw_StopListeningToSettings(settings: ref<IScriptable>, system: ref<IScriptable>) {
  ModSettings.UnregisterListenerToClass(settings);
  ModSettings.UnregisterListenerToModifications(system);
}

@if(!ModuleExists("ModSettingsModule"))
public func DebugDraw_StopListeningToSettings(settings: ref<IScriptable>, system: ref<IScriptable>) {}

// Saves a Bool setting through Mod Settings so its page shows it; nothing without Mod Settings.
@if(ModuleExists("ModSettingsModule"))
public func DebugDraw_SaveSetting(category: CName, name: CName, value: Bool) {
  for configVar in ModSettings.GetVars(n"Debug Draw", category) {
    let boolVar = configVar as ModConfigVarBool;
    if IsDefined(boolVar) && Equals(configVar.GetName(), name) {
      boolVar.SetValue(value);
      ModSettings.AcceptChanges();
      return;
    }
  }
}

@if(!ModuleExists("ModSettingsModule"))
public func DebugDraw_SaveSetting(category: CName, name: CName, value: Bool) {}
