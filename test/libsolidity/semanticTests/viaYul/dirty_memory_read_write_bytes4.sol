contract C {
	function f() public pure returns (bool correct) {
		uint256[1] memory m;
		bytes[4] memory x;
		assembly {
			mstore(m, 0xdeadbeef15dead)
		}
		x = m[0];
		bool a = (m[0] == 0xdeadbeef15dead);
		bool b = (x == 0xef15dead);
		correct = a && b;
	}
}
// ====
// compileViaYul: also
// ----
// f() -> true, true
