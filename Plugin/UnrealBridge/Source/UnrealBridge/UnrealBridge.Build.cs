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
			"GameplayTagsEditor",
			"GameplayTasks",
			"GameplayTasksEditor",
			"MainFrame",
			"NavigationSystem",
			"Navmesh",
			"Landscape",
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
			"StructUtils",
			// Geometry Script — Lane 2 of the procedural-content roadmap
			// (UnrealBridgeGeometryLibrary). UDynamicMesh + the runtime BP
			// function libs (CopyMeshFromStaticMesh, ApplyMeshBoolean, etc.)
			// live in GeometryScriptingCore / GeometryFramework; the editor-
			// only asset-creation lib (CreateNewStaticMeshAssetFromMesh) is
			// in GeometryScriptingEditor — UnrealBridge is editor-only so
			// linking the editor module is fine.
			"GeometryScriptingCore",
			"GeometryFramework",
			"GeometryScriptingEditor",
			// TraceLog hosts UE::Trace::EnumerateChannels (used by M4-4
			// list_trace_channels). Core publicly forwards TraceLog headers
			// but the symbols are __declspec(dllimport) so a direct link
			// dep is required. TraceLog.Build.cs sets
			// bRequiresImplementModule=false, so this only pulls the link
			// import — no extra runtime cost.
			"TraceLog",
		});

		// Live Coding is a Windows-only editor module. Guard the dep so
		// non-Windows builds of this editor plugin don't fail to link.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
