using System;

// A .NET 8 console program that throws and catches a managed exception and a runtime one, then returns 42.
static class Program
{
	static int Main()
	{
		Console.WriteLine("start " + Environment.Version + " " + (IntPtr.Size * 8) + "-bit");
		try {
			throw new InvalidOperationException("boom");
		} catch (InvalidOperationException error) {
			Console.WriteLine("caught " + error.Message);
		}
		try {
			var values = new int[1];
			values[values.Length + 1] = 1;
		} catch (IndexOutOfRangeException) {
			Console.WriteLine("caught index");
		}
		Console.WriteLine("done");
		return 42;
	}
}
