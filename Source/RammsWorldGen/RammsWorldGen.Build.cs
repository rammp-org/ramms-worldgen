// Copyright ATDev. All Rights Reserved.

using UnrealBuildTool;

// Seed-driven generation of pedestrian / wheelchair environments on top of
// PCG. The module is Runtime so a packaged or -game sim can regenerate a world
// per episode; everything editor-only (graph authoring, level assembly, the
// MCP toolset) lives in Content/Python and never links into this module.
public class RammsWorldGen : ModuleRules
{
	public RammsWorldGen(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// The same preset + seed must build the same world in the editor and in a packaged game. Editor
		// targets compile with /fp:precise but game targets default to /fp:fast, which reassociates and
		// contracts floating-point maths differently, so layout decisions on near-ties (a shelf that just
		// fits, a random yaw a ulp apart) came out differently in a cooked build. Pin precise semantics.
		FPSemantics = FPSemanticsMode.Precise;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"PCG",                 // UPCGSettings / IPCGElement / point, spline and param data
			"GeometryCore",        // FDynamicMesh3
			"GeometryFramework",   // UDynamicMesh / UDynamicMeshComponent
			"GeometryScriptingCore", // UGeometryScriptLibrary_* primitive and mesh functions
			"Json",
			"JsonUtilities",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"PhysicsCore",         // FBodyInstance: which bodies of a held player were simulating
		});

		if (Target.bBuildEditor)
		{
			// URammsWorldGenBridge::OpenMap loads a map into the editor when no game is running.
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd" });
			// URammsWorldGenGraphLibrary::CaptureGraphPanel: screenshots of the PCG graph editor for docs.
			PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore", "GraphEditor" });
		}
	}
}
