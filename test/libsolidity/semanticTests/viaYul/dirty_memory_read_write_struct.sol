contract C {
	function f() public pure returns (bool correct) {
		uint256[3] memory m;
		struct S {
			uint32 m1;
			int16 m2;
			bytes3 m3;
		}
		S memory x;
		assembly {
			mstore(m, 0xdeadbeef15dead)
			mstore(add(m, 32), 0xacce551b1e)
			mstore(add(m, 64), 0xc1a551ca1)
		}
		x.m1 = m[0];
		x.m2 = m[1];
		x.m3 = m[2];
		bool a = (m[0] == 0xdeadbeef15dead);
		bool b = (x.m1 == 0xef15dead);
		bool c = (m[1] == 0xacce551b1e);
		bool d = (x.m2 == 0x1b1e);
		bool e = (m[2] == 0xc1a551ca1);
		bool f = (x.m3 == 0x551ca1);
		correct = a && b && c && d && e && f;
	}
}
// ====
// compileViaYul: also
// ----
// f() -> true
