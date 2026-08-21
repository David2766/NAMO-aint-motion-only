type FloorplanWizardStep = "image" | "ocr" | "rooms" | "radar" | "final";
type FloorplanWizardDirection = "forward" | "back";

interface FloorplanWizardNavigationOptions {
  transitionMs?: number;
}

interface FloorplanWizardText {
  stepTitle: Record<FloorplanWizardStep, string>;
  stepDescription: Record<FloorplanWizardStep, string>;
}

export function createFloorplanWizardNavigation({
  transitionMs = 220
}: FloorplanWizardNavigationOptions = {}) {
  let step = $state<FloorplanWizardStep>("image");
  let animating = $state(false);
  let direction = $state<FloorplanWizardDirection>("forward");
  let timer: ReturnType<typeof window.setTimeout> | null = null;

  function transition(
    nextStep: FloorplanWizardStep,
    nextDirection: FloorplanWizardDirection,
    afterTransition?: () => void
  ): void {
    direction = nextDirection;
    animating = true;
    clearTimer();
    timer = window.setTimeout(() => {
      step = nextStep;
      animating = false;
      timer = null;
      afterTransition?.();
    }, transitionMs);
  }

  function replace(
    nextStep: FloorplanWizardStep,
    nextDirection: FloorplanWizardDirection,
    afterTransition?: () => void
  ): void {
    clearTimer();
    direction = nextDirection;
    step = nextStep;
    animating = false;
    afterTransition?.();
  }

  function reset(): void {
    clearTimer();
    step = "image";
    animating = false;
    direction = "forward";
  }

  function destroy(): void {
    clearTimer();
  }

  function title(text: FloorplanWizardText): string {
    return text.stepTitle[step];
  }

  function description(text: FloorplanWizardText): string {
    return text.stepDescription[step];
  }

  function clearTimer(): void {
    if (!timer) return;
    window.clearTimeout(timer);
    timer = null;
  }

  return {
    get step() {
      return step;
    },
    get animating() {
      return animating;
    },
    get direction() {
      return direction;
    },
    transition,
    replace,
    reset,
    destroy,
    title,
    description
  };
}
