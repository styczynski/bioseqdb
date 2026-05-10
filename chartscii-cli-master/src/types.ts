import { CustomizationOptions, InputData } from 'chartscii';

export type CLIOptions = CustomizationOptions & {
  _?: Array<string | number>;
  $0?: string;
}

export interface InputSource {
  type: 'stdin' | 'file' | 'args';
  data: string | string[];
}

export interface ParsedInput {
  data: InputData[];
  options: CustomizationOptions;
}

export { InputData, CustomizationOptions };
