contract C {
    function f() public pure {
        uint8 a;
        a ** 1E5;
    }

    function g() public pure {
        int a;
        a ** 1E1233;
    }
}
// ----
// TypeError: (74-77): Literal is too large. The result of the operation has type: (uint8).
// TypeError: (145-151): Literal is too large. The result of the operation has type: (int256).
