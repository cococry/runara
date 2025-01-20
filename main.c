#include <stdio.h>
#include <GLFW/glfw3.h>

int main() {

  glfwInit();

  GLFWwindow* win = glfwCreateWindow(800, 600, "window", NULL , NULL);

  while(1);
  glfwTerminate();
  printf("Hello, World!\n");
}
