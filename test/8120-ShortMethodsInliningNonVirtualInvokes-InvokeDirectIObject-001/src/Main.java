class Main {
    final static int iterations = 10;
    
    public static void main(String[] args) {
        Test test = new Test();
        Foo nextThingy = new Foo();

        System.out.println("Initial nextThingy value is " + nextThingy.getClass().toString());

        for(int i = 0; i < iterations; i++) {
            nextThingy = test.gimme();
            test.hereyouare(nextThingy);
        }

        System.out.println("Final nextThingy value is " + nextThingy.getClass().toString());
        System.out.println("test.thingies value is " + test.thingies.getClass().toString());

    }
}
