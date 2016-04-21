/*
 * Copyright (c) 2014, 2015, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

/*
 * @test
 * @bug 8033626
 * @summary assert(ex_map->jvms()->same_calls_as(_exceptions->jvms())) failed: all collected exceptions must come from the same place
 * @library /testlibrary
 * @run main/othervm -XX:-TieredCompilation -Xbatch -XX:CompileOnly=TestObjectClone::f TestObjectClone
 */
import jdk.test.lib.Asserts;

public class TestObjectClone implements Cloneable {
    static class A extends TestObjectClone {}
    static class B extends TestObjectClone {
        public B clone() {
            return (B)TestObjectClone.b;
        }
    }
    static class C extends TestObjectClone {
        public C clone() {
            return (C)TestObjectClone.c;
        }
    }
    static class D extends TestObjectClone {
        public D clone() {
            return (D)TestObjectClone.d;
        }
    }
    static TestObjectClone a = new A(), b = new B(), c = new C(), d = new D();

    public static Object f(TestObjectClone o) throws CloneNotSupportedException {
        // Polymorphic call site: >90% Object::clone / <10% other methods
        return o.clone();
    }

    public static void main(String[] args) throws Exception {
        TestObjectClone[] params1 = {a, a, a, a, a, a, a, a, a, a, a,
                          a, a, a, a, a, a, a, a, a, a, a,
                          a, a, a, a, a, a, a, a, a, a, a,
                          b, c, d};

        for (int i = 0; i < 15000; i++) {
            f(params1[i % params1.length]);
        }

        Asserts.assertTrue(f(a) != a);
        Asserts.assertTrue(f(b) == b);
        Asserts.assertTrue(f(c) == c);
        Asserts.assertTrue(f(d) == d);

        try {
            f(null);
            throw new AssertionError("");
        } catch (NullPointerException e) { /* expected */ }

        System.out.println("TEST PASSED");
    }
}