class Main {
    final static int iterations = 10;

    public static void main(String[] args) {
        Test test = new Test();
        float workJ = 256.0f;
        float workK = 1024.0f;

        System.out.println("Initial workJ value is " + workJ);

        for(float i = 0.0f; i < iterations; i++) {
            workJ = test.simple_method(workJ, workK) + i;
        }

        System.out.println("Final workJ value is " + workJ);
    }
}
