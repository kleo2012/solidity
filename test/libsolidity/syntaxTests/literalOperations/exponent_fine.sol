contract C {
    function f() public pure {
        uint24 a;
        a ** 1E5;
        a = a ** 1E5;
        a = 0 ** 1E1233;
    }
}
