{
    files = {
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\BannerManager.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\BlockRemoveHook.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\Database.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\DebugCommand.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\ExchangeManager.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\ExplosionHook.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\FireBurnHook.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\GachaEngine.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\GuildManager.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\LootPoolManager.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\MailManager.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\MemoryOperators.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\MyMod.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\PlayerProfile.cpp.obj]],
        [[build\.objs\GachaPlugin\windows\x64\release\src\mod\QuestManager.cpp.obj]]
    },
    values = {
        [[C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.50.35717\bin\HostX64\x64\link.exe]],
        {
            "-nologo",
            "-machine:x64",
            [[-libpath:build\.prelink\lib]],
            [[-libpath:C:\Users\Iya Aja Gw Mah\AppData\Local\.xmake\packages\l\levilamina\26.10.8\21c8b256ae9044cc9f56ffdb4b30d1ec\lib]],
            [[-libpath:C:\Users\Iya Aja Gw Mah\AppData\Local\.xmake\packages\f\fmt\11.2.0\ee1902dd7cff4195a287949f80dbce70\lib]],
            [[-libpath:C:\Users\Iya Aja Gw Mah\AppData\Local\.xmake\packages\l\leveldb\1.23\2532d000dc9d47cbb618c80ba0e6d806\lib]],
            [[-libpath:C:\Users\Iya Aja Gw Mah\AppData\Local\.xmake\packages\s\snappy\1.2.2\b3244272357b4994abb43dc0aa939d28\lib]],
            [[-libpath:C:\Users\Iya Aja Gw Mah\AppData\Local\.xmake\packages\s\symbolprovider\v1.2.0\716edebcb0f14dbbbc97a0d6aa352593\lib]],
            "/opt:ref",
            "/opt:icf",
            "-debug",
            [[-pdb:build\windows\x64\release\GachaPlugin.pdb]],
            "bedrock_runtime_api.lib",
            "LeviLamina.lib",
            "fmt.lib",
            "leveldb.lib",
            "snappy.lib",
            "SymbolProvider.lib",
            "/DELAYLOAD:bedrock_runtime.dll"
        }
    }
}