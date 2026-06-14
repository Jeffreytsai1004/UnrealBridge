using UnrealBuildTool;

public class UnrealBridge : ModuleRules
{
	public UnrealBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Sockets",
			"Networking",
			"Json",
			"JsonUtilities",
			"PythonScriptPlugin",
			"DeveloperSettings",
			"BlueprintGraph",
			"KismetCompiler",
			"UMG",
			"AssetRegistry",
			"Kismet",
			"GraphEditor",
			"UMGEditor",
			"AnimGraph",
			"UnrealEd",
			"EditorSubsystem",
			"MovieScene",
			"AnimGraphRuntime",
			"ContentBrowser",
			"ContentBrowserData",
			"LevelEditor",
			"GameplayAbilities",
			"GameplayAbilitiesEditor",
			"GameplayTags",
			"GameplayTasks",
			"GameplayTasksEditor",
			"MainFrame",
			"NavigationSystem",
			"Navmesh",
			"EnhancedInput",
			"InputBlueprintNodes",
			"InputEditor",
			"Projects",
			"Slate",
			"SlateCore",
			"InputCore",
			"SourceControl",
			"ImageCore",
			"ImageWrapper",
			"RenderCore",
			"RHI",
			"MaterialEditor",
			"PoseSearch",
			"Chooser",
			"ChooserEditor",
			// TraceLog hosts UE::Trace::EnumerateChannels (used by M4-4
			// list_trace_channels). Core publicly forwards TraceLog headers
			// but the symbols are __declspec(dllimport) so a direct link
			// dep is required. TraceLog.Build.cs sets
			// bRequiresImplementModule=false, so this only pulls the link
			// import — no extra runtime cost.
			"TraceLog",
		});

		// WITH_GAMEPLAYTAGSEDITOR=0 because GameplayTagsEditor is an optional plugin
		// that only exists in UE 5.5+ / Marketplace bundles. The C++ code uses
		// IGameplayTagsEditorModule::IsAvailable() at runtime as the safety gate.
		PrivateDefinitions.Add("WITH_GAMEPLAYTAGSEDITOR=0");


		// Live Coding is a Windows-only editor module. Guard the dep so
		// non-Windows builds of this editor plugin don't fail to link.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
