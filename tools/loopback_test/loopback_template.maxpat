{
	"patcher" : {
		"fileversion" : 1,
		"appversion" : {"major" : 9, "minor" : 2, "revision" : 0, "architecture" : "x64"},
		"rect" : [0.0, 0.0, 640.0, 480.0],
		"gridsize" : [15.0, 15.0],
		"boxes" : [
			{"box" : {"id" : "obj-1", "maxclass" : "newobj", "text" : "vst~ \"__OUT__\" 2 2", "numinlets" : 3, "numoutlets" : 3, "outlettype" : ["signal", "signal", ""], "patching_rect" : [20.0, 20.0, 480.0, 22.0]}},
			{"box" : {"id" : "obj-2", "maxclass" : "newobj", "text" : "vst~ \"__IN__\" 2 2", "numinlets" : 3, "numoutlets" : 3, "outlettype" : ["signal", "signal", ""], "patching_rect" : [20.0, 80.0, 480.0, 22.0]}},
			{"box" : {"id" : "obj-3", "maxclass" : "newobj", "text" : "*~ 0.", "numinlets" : 2, "numoutlets" : 2, "outlettype" : ["signal", "signal"], "patching_rect" : [20.0, 140.0, 80.0, 22.0]}},
			{"box" : {"id" : "obj-4", "maxclass" : "newobj", "text" : "dac~ 1 2", "numinlets" : 2, "numoutlets" : 0, "outlettype" : [], "patching_rect" : [20.0, 200.0, 80.0, 22.0]}},
			{"box" : {"id" : "obj-5", "maxclass" : "newobj", "text" : "loadbang", "numinlets" : 1, "numoutlets" : 1, "outlettype" : ["bang"], "patching_rect" : [600.0, 20.0, 80.0, 22.0]}},
			{"box" : {"id" : "obj-6", "maxclass" : "newobj", "text" : "delay 1500", "numinlets" : 1, "numoutlets" : 1, "outlettype" : ["bang"], "patching_rect" : [600.0, 60.0, 80.0, 22.0]}},
			{"box" : {"id" : "obj-7", "maxclass" : "message", "text" : "start", "numinlets" : 2, "numoutlets" : 1, "outlettype" : [""], "patching_rect" : [600.0, 100.0, 80.0, 22.0]}}
		],
		"lines" : [
			{"patchline" : {"source" : ["obj-1", 0], "destination" : ["obj-2", 0]}},
			{"patchline" : {"source" : ["obj-1", 1], "destination" : ["obj-2", 1]}},
			{"patchline" : {"source" : ["obj-2", 0], "destination" : ["obj-3", 0]}},
			{"patchline" : {"source" : ["obj-2", 1], "destination" : ["obj-3", 1]}},
			{"patchline" : {"source" : ["obj-3", 0], "destination" : ["obj-4", 0]}},
			{"patchline" : {"source" : ["obj-3", 1], "destination" : ["obj-4", 1]}},
			{"patchline" : {"source" : ["obj-5", 0], "destination" : ["obj-6", 0]}},
			{"patchline" : {"source" : ["obj-6", 0], "destination" : ["obj-7", 0]}},
			{"patchline" : {"source" : ["obj-7", 0], "destination" : ["obj-4", 0]}}
		]
	}
}
