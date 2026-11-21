autowatch = 1;

// vst~ sends MIDI as a list of raw bytes ("144 60 100"); amxd~ wants the
// byte stream that [midiin] would produce, i.e. one integer per message.
function anything()
{
	var a = arrayfromargs(arguments);
	for (var i = 0; i < a.length; i++)
		outlet(0, a[i]);
}

function list()
{
	anything.apply(this, arguments);
}