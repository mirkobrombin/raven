// The JScript script of the test package: the language, the installer's session, its records and messages.

function JMain() {
	var parts = "3,4,5".split(","), total = 0;
	for (var i = 0; i < parts.length; i++)
		total += parseInt(parts[i], 10);
	var text = Session.Property("ProductName").substr(0, 5).toUpperCase() + "-" + total;
	try {
		null.x;
	} catch (e) {
		text += "-caught";
	}
	var record = Session.Installer.CreateRecord(1);
	record.StringData(1) = "A message from the JScript action";
	Session.Message(0x04000000, record);
	if (Session.EvaluateCondition("ProductName = \"Raven Script\"") == 1)
		text += "-condition";
	Session.Property("JSCRIPT_RESULT") = text + "-" + [1, 2, 3].join("+");
	return 1;
}
