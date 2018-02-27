// The test checks that stack after NullPointerException occurs is correct despite inlining
class Main {
    final static int iterations = 10;

//    public static int getThingies(int i) {
//        return thingiesArray[i];
//    }
//  |[000194] Main.getThingies:(I)I
//  |0000: sget-object v0, LMain;.thingiesArray:[I // field@0001
//  |0002: aget v0, v0, v1
//  |0004: return v0

    public static void main(String[] args) {
        Test test = new Test(iterations);
        boolean nextThingy = false;
        boolean sumArrElements = false;
        for(int i = 0; i < iterations; i++) {
            sumArrElements = sumArrElements & test.thingiesArray[i];
        }

        try {
            for(int i = 0; i < iterations; i++) {
                nextThingy = test.gimme(test.thingiesArray, i) || true;
                test.hereyouare(test.thingiesArray, nextThingy, i);
            }
        } catch (Exception e) {
            throw e;
        } finally {
        }
    }
}
